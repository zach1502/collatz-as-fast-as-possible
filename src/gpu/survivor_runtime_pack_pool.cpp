#include "survivor_runtime/internal.hpp"

#include <cstring>
#include <mutex>

namespace collatz::gpu {

using namespace survivor_detail;

// Creates the producer-pack pool (survivor_runtime/internal.hpp), on the dispatch
// or main thread before the producer threads start (the spec it copies is
// read-only afterwards). `trim_tables` means the caller will install
// set_trim_tables(fmin, fcount entries). Returns false, creating no pool, when
// there are no blocks to make.
bool SurvivorRunner::init_pack_pool(
    bool trim_tables, unsigned fmin, std::size_t fcount, std::size_t max_n, std::size_t nblocks) {
    release_pack_pool();
    if (max_n == 0 || nblocks == 0) {
        return false;
    }
    // The batch-independent part of trim_bits_for, as configured now. A later
    // change only makes take_packed see different bits and fall back.
    const bool trim_ok = trim_tables && fcount != 0 && fcount <= 256;
    auto* p = new PackPool;
    p->pinned = !xfer_.pinned_failed;
    p->trim_base = trim_ok ? 1u : 0u;
    p->fmin = fmin;
    p->fcount = fcount;
    // Block size: the narrowest layout (n0/m as u64 + astar + 1-byte f index)
    // when the f table applies, else the widest. A batch whose values force a
    // wider layout than the block holds is packed on the dispatch thread.
    p->bytes = trim_ok ? 2 * staging_round_up(max_n * sizeof(cl_ulong)) +
                             staging_round_up(max_n * sizeof(cl_uint)) + staging_round_up(max_n)
                       : staged_full_bytes(max_n);
    p->blocks.resize(nblocks);
    for (std::size_t i = 0; i < nblocks; ++i) {
        PackPool::Block& b = p->blocks[i];
        if (p->pinned) {
            cl_int status = CL_SUCCESS;
            b.buf = clCreateBuffer(
                context_, CL_MEM_ALLOC_HOST_PTR | CL_MEM_READ_WRITE, p->bytes, nullptr, &status);
            if (status == CL_SUCCESS) {
                void* h = clEnqueueMapBuffer(queue_,
                                             b.buf,
                                             CL_TRUE,
                                             CL_MAP_WRITE,
                                             0,
                                             p->bytes,
                                             0,
                                             nullptr,
                                             nullptr,
                                             &status);
                if (status == CL_SUCCESS && h != nullptr) {
                    b.host = static_cast<unsigned char*>(h);
                } else {
                    clReleaseMemObject(b.buf);
                    b.buf = nullptr;
                }
            } else {
                b.buf = nullptr;
            }
            if (b.host == nullptr) {
                // Out of pinned memory: keep the blocks that exist.
                p->blocks.resize(i);
                break;
            }
        } else {
            b.heap.resize(p->bytes);
            b.host = b.heap.data();
        }
        p->free_list.push_back(static_cast<int>(i));
    }
    if (p->blocks.empty()) {
        delete p;
        return false;
    }
    p->stats.blocks = p->blocks.size();
    p->stats.block_bytes = p->bytes;
    p->stats.pinned = p->pinned;
    pack_pool_ = p;
    return true;
}

// Producer thread (thread-safe): packs `batch` into a free block and sets
// batch.pp_block / pp_gen. Returns false when nothing was packed.
bool SurvivorRunner::pack_on_producer(SurvivorBatch& batch) {
    PackPool* const p = pack_pool_;
    const std::size_t n = batch.size();
    if (p == nullptr || n == 0 || batch.f0.size() != n || batch.astar.size() != n ||
        batch.m.size() != n) {
        return false;
    }
    // trim_bits_for for this batch, from the snapshotted spec.
    cl_uint bits = p->trim_base;
    if (bits != 0u) {
        if (batch.hint_n0_hi == 0u) {
            bits |= 2u;
        }
        if (batch.hint_m_hi == 0u) {
            bits |= 4u;
        }
    }
    int idx = -1;
    {
        std::lock_guard<std::mutex> lk(p->mtx);
        if (p->free_list.empty()) {
            ++p->stats.nopool;
            return false;
        }
        idx = p->free_list.back();
        p->free_list.pop_back();
        PackPool::Block& b = p->blocks[static_cast<std::size_t>(idx)];
        b.st = PackPool::St::Packing;
        ++b.gen;
    }
    PackPool::Block& b = p->blocks[static_cast<std::size_t>(idx)];
    // Exclusive owner of b until it is published as Packed below.
    const cl_uint bits_try = bits;
    const int nw = pack_staged_core(batch, bits, b.host, p->bytes, b.w, p->fmin, p->fcount);
    std::lock_guard<std::mutex> lk(p->mtx);
    if (nw < 0) {
        b.st = PackPool::St::Free;
        p->free_list.push_back(idx);
        ++p->stats.nofit;
        return false;
    }
    b.nw = nw;
    b.bits_try = bits_try;
    b.bits = bits;
    b.ptr = batch.n0.data();
    b.n = n;
    b.first = batch.n0[0];
    b.last = batch.n0[n - 1];
    b.st = PackPool::St::Packed;
    batch.pp_block = idx;
    batch.pp_gen = b.gen;
    ++p->stats.packed;
    return true;
}

// Takes `batch`'s producer-packed block for an upload (dispatch thread). Returns
// false when there is no valid block; the caller then packs on the dispatch
// thread.
bool SurvivorRunner::take_packed(const SurvivorBatch& batch, cl_uint bits_try, PackedBlock& t) {
    PackPool* const p = pack_pool_;
    if (p == nullptr || batch.pp_block < 0) {
        return false;
    }
    reclaim_packed();
    const std::size_t n = batch.size();
    std::lock_guard<std::mutex> lk(p->mtx);
    if (static_cast<std::size_t>(batch.pp_block) >= p->blocks.size()) {
        ++p->stats.stale;
        return false;
    }
    PackPool::Block& b = p->blocks[static_cast<std::size_t>(batch.pp_block)];
    // Same block generation, still Packed (not yet uploaded), and the same
    // vectors + first/last n0 (a copied SurvivorBatch has other vectors).
    if (b.gen != batch.pp_gen || b.st != PackPool::St::Packed || n == 0 ||
        b.ptr != batch.n0.data() || b.n != n ||
        std::memcmp(&b.first, &batch.n0[0], sizeof(cl_uint4)) != 0 ||
        std::memcmp(&b.last, &batch.n0[n - 1], sizeof(cl_uint4)) != 0) {
        ++p->stats.stale;
        return false;
    }
    if (b.bits_try != bits_try) {
        // The runner would try another layout now (config changed after
        // init_pack_pool): pack on the dispatch thread; the block is not needed.
        b.st = PackPool::St::Free;
        p->free_list.push_back(batch.pp_block);
        ++p->stats.mismatch;
        return false;
    }
    b.st = PackPool::St::InFlight;
    b.ev = nullptr;
    t.block = batch.pp_block;
    t.nw = b.nw;
    t.bits = b.bits;
    ++p->stats.hits;
    return true;
}

const SurvivorRunner::StagedWrite* SurvivorRunner::packed_writes(int block) const noexcept {
    // Dispatch thread, block InFlight (owned by the upload in progress).
    return pack_pool_->blocks[static_cast<std::size_t>(block)].w;
}

void SurvivorRunner::set_packed_event(int block, cl_event ev) noexcept {
    // Dispatch thread only: InFlight blocks are never touched by producers.
    pack_pool_->blocks[static_cast<std::size_t>(block)].ev = ev;
}

void SurvivorRunner::reclaim_packed() {
    PackPool* const p = pack_pool_;
    if (p == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lk(p->mtx);
    for (std::size_t i = 0; i < p->blocks.size(); ++i) {
        PackPool::Block& b = p->blocks[i];
        if (b.st != PackPool::St::InFlight) {
            continue;
        }
        if (b.ev != nullptr) {
            cl_int es = CL_SUCCESS;
            const cl_int q =
                clGetEventInfo(b.ev, CL_EVENT_COMMAND_EXECUTION_STATUS, sizeof(es), &es, nullptr);
            if (q != CL_SUCCESS) {
                continue; // keep it busy; drained at release
            }
            if (es < 0) {
                check(es, "pack block write");
            }
            if (es != CL_COMPLETE) {
                continue;
            }
            clReleaseEvent(b.ev);
            b.ev = nullptr;
        }
        b.st = PackPool::St::Free;
        p->free_list.push_back(static_cast<int>(i));
    }
}

// Dispatch thread, once `batch` is finished: returns its block if it was never
// uploaded, and reclaims blocks whose last write has completed.
void SurvivorRunner::retire_packed(SurvivorBatch& batch) {
    PackPool* const p = pack_pool_;
    if (p == nullptr) {
        return;
    }
    if (batch.pp_block >= 0) {
        std::lock_guard<std::mutex> lk(p->mtx);
        const auto bi = static_cast<std::size_t>(batch.pp_block);
        if (bi < p->blocks.size()) {
            PackPool::Block& b = p->blocks[bi];
            if (b.gen == batch.pp_gen && b.st == PackPool::St::Packed && b.ptr == batch.n0.data()) {
                // Never uploaded from: no command reads it.
                b.st = PackPool::St::Free;
                p->free_list.push_back(batch.pp_block);
                ++p->stats.unused;
            }
        }
        batch.pp_block = -1;
        batch.pp_gen = 0;
    }
    reclaim_packed();
}

SurvivorRunner::PackPoolStats SurvivorRunner::pack_pool_stats() const {
    if (pack_pool_ == nullptr) {
        return PackPoolStats{};
    }
    std::lock_guard<std::mutex> lk(pack_pool_->mtx);
    return pack_pool_->stats;
}

void SurvivorRunner::release_pack_pool() noexcept {
    PackPool* const p = pack_pool_;
    if (p == nullptr) {
        return;
    }
    // Callers drain both queues first (release_transfer), so every block write has
    // completed; waiting on any remaining event is then immediate.
    for (PackPool::Block& b : p->blocks) {
        if (b.ev != nullptr) {
            clWaitForEvents(1, &b.ev);
            clReleaseEvent(b.ev);
            b.ev = nullptr;
        }
        if (b.buf != nullptr) {
            if (queue_ != nullptr) {
                clEnqueueUnmapMemObject(queue_, b.buf, b.host, 0, nullptr, nullptr);
                clFinish(queue_);
            }
            clReleaseMemObject(b.buf);
            b.buf = nullptr;
        }
        b.host = nullptr;
    }
    delete p;
    pack_pool_ = nullptr;
}

} // namespace collatz::gpu
