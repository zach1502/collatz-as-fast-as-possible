#include "host_pipeline.hpp"
#include "survivor_runtime/internal.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace collatz::gpu {

using namespace survivor_detail;

namespace {

// Hard cap on the candidate-id buffer, in 4-byte ids (2^29 ids = 2 GiB). The
// buffer is sized to the exact host bound sum(min(a_star, 2^A)), clamped to the
// cap. A mask id at or past the cap sets cand_ovf (CpiResult::cand_overflow)
// and the caller splits the batch by survivors; large runs reach this path.
constexpr std::size_t kCpiCandHardCap = static_cast<std::size_t>(1) << 29;

// 2^29 ids are 2,147,483,648 B, just above CL_DEVICE_MAX_MEM_ALLOC_SIZE on some
// devices (an RTX 3070 reports 2,147,352,576 B), and a strict driver may refuse
// the buffer. So the id count is clamped to one allocation, rounded down to
// kCpiCandCapGrain ids: a multiple of every mask work-group size (<= 256) that
// keeps the byte size a multiple of 1 KiB. The kernels check the cap per id, so
// any cap is correct. A lower cap only makes batches split more often, and only
// on devices whose max_alloc is below 2 GiB; results never change.
constexpr std::size_t kCpiCandCapGrain = 256;
[[nodiscard]] std::size_t cpi_cand_alloc_cap(std::uint64_t max_alloc) noexcept {
    return cand_alloc_cap(max_alloc, sizeof(cl_uint), kCpiCandHardCap, kCpiCandCapGrain);
}

// Effective candidate-buffer cap: kCpiCandHardCap clamped to one allocation,
// optionally lowered by the test hook COLLATZ_GPU_CPI_CAND_CAP. Read on every
// call so a test can set it for its own run.
[[nodiscard]] std::size_t cpi_cand_hard_cap(std::uint64_t max_alloc) {
    // COLLATZ_GPU_MAX_ALLOC is the test hook verify_gpu's bitvector split also
    // reads. It can only lower the device limit, and lets a device with a
    // larger limit exercise the allocation clamp and the batch splits it causes.
    if (const char* e = std::getenv("COLLATZ_GPU_MAX_ALLOC")) {
        const std::uint64_t forced = std::strtoull(e, nullptr, 10);
        if (forced > 0 && forced < max_alloc) {
            max_alloc = forced;
        }
    }
    const std::size_t alloc_cap = cpi_cand_alloc_cap(max_alloc);
    std::size_t cap = alloc_cap;
    if (const char* e = std::getenv("COLLATZ_GPU_CPI_CAND_CAP")) {
        const unsigned long long v = std::strtoull(e, nullptr, 10);
        if (v > 0 && v < kCpiCandHardCap) {
            cap = std::min(static_cast<std::size_t>(v), alloc_cap);
        }
    }
    return cap;
}

} // namespace

// The candidate-id cap every cpi route uses: the max_alloc clamp (and
// COLLATZ_GPU_CPI_CAND_CAP), lowered further by the device-memory budget from
// set_cand_budget_cap (0 = no budget).
std::size_t SurvivorRunner::cand_id_cap() const {
    const std::size_t cap = cpi_cand_hard_cap(info_.max_mem_alloc_size);
    return cand_budget_cap_ != 0 && cand_budget_cap_ < cap ? cand_budget_cap_ : cap;
}

std::size_t SurvivorRunner::cand_id_bytes() const noexcept {
    return sizeof(cl_uint); // cpi_cand_buf_
}

void SurvivorRunner::release_io_buffers() noexcept {
    for (cl_mem* b : {&n0_buf_,
                      &m_buf_,
                      &astar_buf_,
                      &inv3f_buf_,
                      &bvbase_buf_,
                      &bvmode_buf_,
                      &p3f_buf_,
                      &cov_buf_,
                      &dig_buf_,
                      &blocks_buf_,
                      &cand_buf_,
                      &maxv_buf_,
                      &ovf_buf_,
                      &pc_buf_,
                      &chk_buf_,
                      &xfer_.act_fidx}) {
        if (*b != nullptr) {
            clReleaseMemObject(*b);
            *b = nullptr;
        }
    }
    capacity_ = 0;
    xfer_.act_cap = 0;
    xfer_.out_cap = 0;
}

// Grows the per-item device buffers to at least `n` items. They are reused across
// run() calls, so the steady state allocates nothing.
void SurvivorRunner::ensure_capacity(std::size_t n) {
    if (n <= capacity_) {
        return;
    }
    release_io_buffers();
    cl_int status = CL_SUCCESS;
    const auto mk = [&](std::size_t bytes, cl_mem_flags flags, const char* what) -> cl_mem {
        cl_mem buf = clCreateBuffer(context_, flags, bytes, nullptr, &status);
        check(status, what);
        return buf;
    };
    n0_buf_ = mk(n * sizeof(cl_uint4), CL_MEM_READ_ONLY, "clCreateBuffer(n0)");
    m_buf_ = mk(n * sizeof(cl_uint4), CL_MEM_READ_ONLY, "clCreateBuffer(m)");
    astar_buf_ = mk(n * sizeof(cl_uint), CL_MEM_READ_ONLY, "clCreateBuffer(astar)");
    inv3f_buf_ = mk(n * sizeof(cl_uint), CL_MEM_READ_ONLY, "clCreateBuffer(inv3f)");
    bvbase_buf_ = mk(n * sizeof(cl_uint), CL_MEM_READ_ONLY, "clCreateBuffer(bvbase)");
    bvmode_buf_ = mk(n * sizeof(cl_uint), CL_MEM_READ_ONLY, "clCreateBuffer(bvmode)");
    p3f_buf_ = mk(n * sizeof(cl_uint4), CL_MEM_READ_ONLY, "clCreateBuffer(p3f)");
    cov_buf_ = mk(n * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "clCreateBuffer(cov)");
    dig_buf_ = mk(n * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "clCreateBuffer(dig)");
    blocks_buf_ = mk(n * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "clCreateBuffer(blocks)");
    cand_buf_ = mk(n * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "clCreateBuffer(cand)");
    maxv_buf_ = mk(n * sizeof(cl_uint4), CL_MEM_WRITE_ONLY, "clCreateBuffer(maxv)");
    ovf_buf_ = mk(n * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "clCreateBuffer(ovf)");
    pc_buf_ = mk(n * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "clCreateBuffer(pc)");
    chk_buf_ = mk(n * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "clCreateBuffer(chk)");
    // Upload trim: 1-byte f index per survivor, part of the active input set.
    xfer_.act_fidx = mk(n * sizeof(cl_uchar), CL_MEM_READ_ONLY, "clCreateBuffer(fidx)");
    capacity_ = n;
    xfer_.act_cap = n;
    xfer_.out_cap = n;
}

void SurvivorRunner::release_cpi_buffers() noexcept {
    for (cl_mem* b : {&cpi_candcnt_buf_,
                      &cpi_candovf_buf_,
                      &cpi_cand_buf_,
                      &cpi_gcov_buf_,
                      &cpi_gdig_buf_,
                      &cpi_gblk_buf_,
                      &cpi_gcnt_buf_,
                      &cpi_gmax_buf_,
                      &cpi_flagcount_buf_,
                      &cpi_flaglist_buf_,
                      &cpi_flagovf_buf_,
                      &cpi_workcnt_buf_,
                      &cpi_gchk_buf_,
                      &sc_words_buf_}) {
        if (*b != nullptr) {
            clReleaseMemObject(*b);
            *b = nullptr;
        }
    }
    cpi_cap_cands_ = 0;
    cpi_cap_groups_ = 0;
    cpi_flag_cap_ = 0;
    cpi_cap_flag_ = 0;
}

// Grows the candidate-per-item buffers to at least `cands` candidates and
// `groups` pass-2 work-groups.
void SurvivorRunner::ensure_cpi_capacity(std::size_t cands, std::size_t groups) {
    cl_int status = CL_SUCCESS;
    const auto mk = [&](std::size_t bytes, cl_mem_flags flags, const char* what) -> cl_mem {
        cl_mem buf = clCreateBuffer(context_, flags, bytes == 0 ? 1 : bytes, nullptr, &status);
        check(status, what);
        return buf;
    };
    // Candidate-sized (packed id list). Grows with the batch's candidate upper
    // bound; the fused mask scatters straight into it.
    if (cands > cpi_cap_cands_) {
        if (cpi_cand_buf_ != nullptr) {
            clReleaseMemObject(cpi_cand_buf_);
            cpi_cand_buf_ = nullptr;
        }
        cpi_cand_buf_ = mk(cands * sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi cand");
        cpi_cap_cands_ = cands;
    }
    // Work-group-sized tail partials.
    if (groups > cpi_cap_groups_) {
        for (cl_mem* b : {&cpi_gcov_buf_, &cpi_gblk_buf_, &cpi_gcnt_buf_, &cpi_gmax_buf_}) {
            if (*b != nullptr) {
                clReleaseMemObject(*b);
                *b = nullptr;
            }
        }
        if (cpi_gdig_buf_ != nullptr) {
            clReleaseMemObject(cpi_gdig_buf_);
            cpi_gdig_buf_ = nullptr;
        }
        cpi_gcov_buf_ = mk(groups * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "cpi gcov");
        cpi_gdig_buf_ = mk(groups * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "cpi gdig");
        cpi_gblk_buf_ = mk(groups * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "cpi gblk");
        cpi_gcnt_buf_ = mk(groups * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "cpi gcnt");
        cpi_gmax_buf_ = mk(groups * sizeof(cl_uint4), CL_MEM_WRITE_ONLY, "cpi gmax");
        if (cpi_gchk_buf_ != nullptr) {
            clReleaseMemObject(cpi_gchk_buf_);
            cpi_gchk_buf_ = nullptr;
        }
        cpi_gchk_buf_ = mk(groups * 4u * sizeof(cl_ulong), CL_MEM_WRITE_ONLY, "cpi gchk");
        cpi_cap_groups_ = groups;
    }
    // Fixed-size control buffers, allocated once.
    if (cpi_candcnt_buf_ == nullptr) {
        cpi_candcnt_buf_ = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi candcnt");
        cpi_candovf_buf_ = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi candovf");
    }
    if (cpi_workcnt_buf_ == nullptr) {
        cpi_workcnt_buf_ = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi workcnt");
    }
    if (sc_words_buf_ == nullptr) {
        sc_words_buf_ = mk(kScWords * sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi sc words");
    }
    if (cpi_flagcount_buf_ == nullptr) {
        cpi_flagcount_buf_ = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi flagcount");
        cpi_flagovf_buf_ = mk(sizeof(cl_uint), CL_MEM_READ_WRITE, "cpi flagovf");
    }
    // Grow the flag list when the requested cap exceeds the allocation.
    if (cpi_flaglist_buf_ == nullptr || cpi_flag_cap_ > cpi_cap_flag_) {
        if (cpi_flaglist_buf_ != nullptr) {
            clReleaseMemObject(cpi_flaglist_buf_);
            cpi_flaglist_buf_ = nullptr;
        }
        const std::size_t fc = cpi_flag_cap_ == 0 ? 1 : cpi_flag_cap_;
        cpi_flaglist_buf_ = mk(fc * sizeof(cl_uint), CL_MEM_WRITE_ONLY, "cpi flaglist");
        cpi_cap_flag_ = cpi_flag_cap_;
    }
}

} // namespace collatz::gpu
