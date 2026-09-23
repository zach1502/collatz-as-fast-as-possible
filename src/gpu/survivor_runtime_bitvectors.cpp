#include "survivor_runtime/internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace collatz::gpu {

using namespace survivor_detail;

// Releases the Step-2 bitvector buffers. Only slots [0, num_buffers) own
// an object: the unused slots alias bv_bufs_[0] (so every kernel bind is a
// valid object), and releasing all four would release buffer 0 up to four
// times. Afterwards every slot is null and num_buffers is 0 until the next
// upload sets it.
static void release_bv_buffers(cl_mem (&bufs)[4], int& num_buffers) noexcept {
    for (int i = 0; i < num_buffers && i < 4; ++i) {
        if (bufs[i] != nullptr) {
            clReleaseMemObject(bufs[i]);
        }
    }
    for (cl_mem& b : bufs) {
        b = nullptr;
    }
    num_buffers = 0;
}

// Writes the Step-2 bitvectors level by level from the host filter's packed
// 64-bit words with clEnqueueWriteBuffer and a clFinish on the main queue. Those
// words have the bytes of the flattened little-endian uint32 table the kernels
// read, so the host must be little-endian. The table is split across
// `num_buffers` device buffers (each holding a power-of-two run of levels,
// `1<<buf_words_log2` words) so no allocation exceeds
// CL_DEVICE_MAX_MEM_ALLOC_SIZE; `num_buffers==1` is one buffer holding the whole
// table, and unused slots alias buffer 0. The transfer happens here, not at the
// first kernel. `levels[i]` points at level i's `level_words32` words.
void SurvivorRunner::upload_bitvector_levels(const std::vector<const void*>& levels,
                                             std::size_t level_words32,
                                             int num_buffers,
                                             cl_uint buf_words_log2) {
    release_bv_buffers(bv_bufs_, num_bv_buffers_);
    num_bv_buffers_ = num_buffers < 1 ? 1 : (num_buffers > 4 ? 4 : num_buffers);
    bv_buf_words_log2_ = buf_words_log2;
    const std::size_t buf_words = static_cast<std::size_t>(1) << buf_words_log2;
    const std::size_t word_count = levels.size() * level_words32;
    // Buffer i holds words [i*buf_words, min((i+1)*buf_words, word_count)).
    for (int i = 0; i < num_bv_buffers_; ++i) {
        const std::size_t off = static_cast<std::size_t>(i) * buf_words;
        const std::size_t n = std::min(buf_words, word_count - off);
        cl_int status = CL_SUCCESS;
        bv_bufs_[i] =
            clCreateBuffer(context_, CL_MEM_READ_ONLY, n * sizeof(std::uint32_t), nullptr, &status);
        check(status, "clCreateBuffer(bv levels)");
    }
    for (int i = num_bv_buffers_; i < 4; ++i) {
        bv_bufs_[i] = bv_bufs_[0];
    }
    // plan_bv_split packs a power-of-two run of whole levels per buffer, so a
    // level never straddles two buffers.
    for (std::size_t l = 0; l < levels.size(); ++l) {
        const std::size_t off = l * level_words32;
        const std::size_t bi = off >> buf_words_log2;
        const std::size_t within = off & (buf_words - 1u);
        if (bi >= static_cast<std::size_t>(num_bv_buffers_) || within + level_words32 > buf_words) {
            throw std::runtime_error("upload_bitvector_levels: level straddles a buffer");
        }
        check(clEnqueueWriteBuffer(queue_,
                                   bv_bufs_[bi],
                                   CL_FALSE,
                                   within * sizeof(std::uint32_t),
                                   level_words32 * sizeof(std::uint32_t),
                                   levels[l],
                                   0,
                                   nullptr,
                                   nullptr),
              "clEnqueueWriteBuffer(bv level)");
    }
    check(clFinish(queue_), "clFinish(bv levels)");
}

// The buffers and bytes of upload_bitvector_levels, streamed through 3 pinned
// staging buffers of `chunk_bytes`. Returns the chunk count; throws on any
// OpenCL failure.
// Chunk c goes through staging slot c % 3: 4 memcpy workers fill it, a
// non-blocking write sends it, and the slot is refilled only after that write
// has completed. On the zero-copy path the source is file-backed mapped pages,
// so this copy is what reads the file, and reading chunk c+1 overlaps the DMA
// of chunk c. Pinned slots also let a discrete GPU DMA directly instead of
// through the driver's pageable staging. The closing clFinish makes the whole
// table visible to kernels on every queue. On failure the caller falls back to
// upload_bitvector_levels, which recreates the device buffers.
std::size_t SurvivorRunner::upload_bitvector_levels_staged(const std::vector<const void*>& levels,
                                                           std::size_t level_words32,
                                                           int num_buffers,
                                                           cl_uint buf_words_log2,
                                                           std::size_t chunk_bytes) {
    constexpr unsigned kSlots = 3;
    constexpr unsigned kThreads = 4;
    release_bv_buffers(bv_bufs_, num_bv_buffers_);
    num_bv_buffers_ = num_buffers < 1 ? 1 : (num_buffers > 4 ? 4 : num_buffers);
    bv_buf_words_log2_ = buf_words_log2;
    const std::size_t buf_words = static_cast<std::size_t>(1) << buf_words_log2;
    const std::size_t word_count = levels.size() * level_words32;
    chunk_bytes = std::max<std::size_t>(chunk_bytes & ~std::size_t{63}, std::size_t{1} << 20);
    struct Slot {
        cl_mem buf = nullptr;
        unsigned char* host = nullptr;
        cl_event ev = nullptr; // the write that last read this slot
    };
    std::vector<Slot> st(kSlots);
    const auto release_all = [&](bool unmap_on_queue) {
        for (Slot& x : st) {
            if (x.ev != nullptr) {
                if (!unmap_on_queue) {
                    clWaitForEvents(1, &x.ev);
                }
                clReleaseEvent(x.ev);
                x.ev = nullptr;
            }
            if (x.buf != nullptr) {
                if (x.host != nullptr) {
                    clEnqueueUnmapMemObject(queue_, x.buf, x.host, 0, nullptr, nullptr);
                }
                clReleaseMemObject(x.buf);
                x.buf = nullptr;
                x.host = nullptr;
            }
        }
    };
    // Test hook: COLLATZ_GPU_EARLY_UPLOAD_INJECT_FAIL=1 throws right after the
    // first chunk's write is enqueued (a write in flight, staging mapped), so a
    // unit test can exercise this cleanup and the caller's fallback.
    const bool inject_fail = [] {
        const char* e = std::getenv("COLLATZ_GPU_EARLY_UPLOAD_INJECT_FAIL");
        return e != nullptr && e[0] != '\0' && e[0] != '0';
    }();
    try {
        for (int i = 0; i < num_bv_buffers_; ++i) {
            const std::size_t off = static_cast<std::size_t>(i) * buf_words;
            const std::size_t n = std::min(buf_words, word_count - off);
            cl_int status = CL_SUCCESS;
            bv_bufs_[i] = clCreateBuffer(
                context_, CL_MEM_READ_ONLY, n * sizeof(std::uint32_t), nullptr, &status);
            check(status, "clCreateBuffer(bv levels, staged)");
        }
        for (int i = num_bv_buffers_; i < 4; ++i) {
            bv_bufs_[i] = bv_bufs_[0];
        }
        for (Slot& x : st) {
            cl_int status = CL_SUCCESS;
            x.buf = clCreateBuffer(
                context_, CL_MEM_ALLOC_HOST_PTR | CL_MEM_READ_WRITE, chunk_bytes, nullptr, &status);
            check(status, "clCreateBuffer(staging)");
            x.host = static_cast<unsigned char*>(clEnqueueMapBuffer(queue_,
                                                                    x.buf,
                                                                    CL_TRUE,
                                                                    CL_MAP_WRITE,
                                                                    0,
                                                                    chunk_bytes,
                                                                    0,
                                                                    nullptr,
                                                                    nullptr,
                                                                    &status));
            check(status, "clEnqueueMapBuffer(staging)");
        }
        // Parallel copy of [src, src + n) into dst (disjoint stripes, joined
        // before the write is enqueued).
        const auto pcopy = [](unsigned char* dst, const unsigned char* src, std::size_t n) {
            if (n < (std::size_t{4} << 20)) {
                std::memcpy(dst, src, n);
                return;
            }
            const std::size_t stripe =
                ((n + kThreads - 1u) / kThreads + 4095u) & ~std::size_t{4095};
            std::vector<std::thread> ws;
            ws.reserve(kThreads);
            std::size_t o = 0;
            try {
                for (; o < n; o += stripe) {
                    const std::size_t len = std::min(stripe, n - o);
                    ws.emplace_back([dst, src, o, len] { std::memcpy(dst + o, src + o, len); });
                }
            } catch (const std::system_error&) { // NOLINT(bugprone-empty-catch): see below
                // Could not start another worker: the stripes from `o` on are
                // copied serially below, after joining the ones that started.
            }
            for (std::thread& w : ws) {
                w.join();
            }
            if (o < n) {
                std::memcpy(dst + o, src + o, n - o);
            }
        };
        std::size_t chunk = 0;
        const std::size_t level_bytes = level_words32 * sizeof(std::uint32_t);
        for (std::size_t l = 0; l < levels.size(); ++l) {
            const std::size_t off = l * level_words32;
            const std::size_t bi = off >> buf_words_log2;
            const std::size_t within = off & (buf_words - 1u);
            if (bi >= static_cast<std::size_t>(num_bv_buffers_) ||
                within + level_words32 > buf_words) {
                throw std::runtime_error(
                    "upload_bitvector_levels_staged: level straddles a buffer");
            }
            const auto* src = static_cast<const unsigned char*>(levels[l]);
            for (std::size_t o = 0; o < level_bytes; o += chunk_bytes, ++chunk) {
                const std::size_t len = std::min(chunk_bytes, level_bytes - o);
                Slot& x = st[chunk % kSlots];
                if (x.ev != nullptr) {
                    check(clWaitForEvents(1, &x.ev), "clWaitForEvents(staging)");
                    clReleaseEvent(x.ev);
                    x.ev = nullptr;
                }
                pcopy(x.host, src + o, len);
                check(clEnqueueWriteBuffer(queue_,
                                           bv_bufs_[bi],
                                           CL_FALSE,
                                           within * sizeof(std::uint32_t) + o,
                                           len,
                                           x.host,
                                           0,
                                           nullptr,
                                           &x.ev),
                      "clEnqueueWriteBuffer(bv level, staged)");
                check(clFlush(queue_), "clFlush(staging)");
                if (inject_fail) {
                    throw std::runtime_error("injected staging failure (test hook)");
                }
            }
        }
        check(clFinish(queue_), "clFinish(staged bv levels)");
        release_all(true);
        check(clFinish(queue_), "clFinish(staging unmap)");
        return chunk;
    } catch (...) {
        release_all(false);
        // Drop the partially written device buffers (owned slots only; the
        // rest alias slot 0) so the caller's fallback upload starts clean.
        clFinish(queue_);
        release_bv_buffers(bv_bufs_, num_bv_buffers_);
        throw;
    }
}

// Binds the bitvector buffers (up to 4) and the split log2 to `kernel` at the
// running arg cursor, in the order the bv-reading kernels declare them:
// bind_bv_first binds bv0 at its slot, bind_bv_rest appends bv1..bv3 and bv_lg.
void SurvivorRunner::bind_bv_first(cl_kernel kernel, cl_uint& arg, const char* tag) {
    check(clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bv_bufs_[0]), tag);
}

void SurvivorRunner::bind_bv_rest(cl_kernel kernel, cl_uint& arg, const char* tag) {
    check(clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bv_bufs_[1]), tag);
    check(clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bv_bufs_[2]), tag);
    check(clSetKernelArg(kernel, arg++, sizeof(cl_mem), &bv_bufs_[3]), tag);
    check(clSetKernelArg(kernel, arg++, sizeof(cl_uint), &bv_buf_words_log2_), tag);
}

} // namespace collatz::gpu
