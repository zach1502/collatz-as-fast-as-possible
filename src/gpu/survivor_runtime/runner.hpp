#ifndef COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_HPP
#define COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_HPP

#include "collatz/gpu/opencl.hpp"
#include "survivor_runtime/batch_types.hpp"
#include "survivor_runtime/state_checks.hpp"
#include "survivor_runtime/state_cpi.hpp"
#include "survivor_runtime/state_device.hpp"
#include "survivor_runtime/state_transfer.hpp"
#include "tail_defaults.hpp" // TailHint

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace collatz::gpu {

/// OpenCL driver for the survivor kernels. Create once (builds the programs and
/// uploads table16), upload the Step-2 bitvectors once per run, then dispatch
/// batches. The base classes hold the state and its accessors; each method is
/// documented at its definition in survivor_runtime*.cpp. The runner owns raw
/// OpenCL handles, so it is neither copied nor moved; create() hands it out by
/// pointer.
class SurvivorRunner : public SurvivorRunnerChecks,
                       public SurvivorRunnerTransfer,
                       public SurvivorRunnerDevice,
                       public SurvivorRunnerCpi {
public:
    /// Builds the programs on the first GPU of gpu_choice() (opencl_runtime.hpp)
    /// that accepts them, trying the next one after a failure; nullptr and *err
    /// when none does.
    [[nodiscard]] static std::unique_ptr<SurvivorRunner> create(std::string* err,
                                                                const TailHint& hint);
    SurvivorRunner(const SurvivorRunner&) = delete;
    SurvivorRunner& operator=(const SurvivorRunner&) = delete;
    SurvivorRunner(SurvivorRunner&&) = delete;
    SurvivorRunner& operator=(SurvivorRunner&&) = delete;
    ~SurvivorRunner();

    /// Uploads the Step-2 bitvectors once, level by level from the host filter's words, split
    /// over `num_buffers`.
    void upload_bitvector_levels(const std::vector<const void*>& levels,
                                 std::size_t level_words32,
                                 int num_buffers,
                                 cl_uint buf_words_log2);
    /// upload_bitvector_levels through pinned staging slots; returns the chunk count.
    std::size_t upload_bitvector_levels_staged(const std::vector<const void*>& levels,
                                               std::size_t level_words32,
                                               int num_buffers,
                                               cl_uint buf_words_log2,
                                               std::size_t chunk_bytes);
    /// Bind bv0 (first), or bv1..bv3 and the split log2 (rest), at the arg cursor `arg`.
    void bind_bv_first(cl_kernel kernel, cl_uint& arg, const char* tag);
    void bind_bv_rest(cl_kernel kernel, cl_uint& arg, const char* tag);
    /// Copies `batch`'s run_cpi inputs into the idle input set on the copy queue.
    void prefetch_inputs(const SurvivorBatch& batch);
    /// Does the host work of a later prefetch_inputs now; enqueues no device command.
    void prefetch_prepack(const SurvivorBatch& batch);
    /// Drops a pending prefetch of the finished `batch` after waiting for its copy.
    void prefetch_forget(const SurvivorBatch& batch);
    /// Blocks until every command on queue_ and the copy queue has completed.
    void drain_transfers() noexcept;
    /// Installs the upload-trim tables p3f[i] = 3^f and fcol[i] for f = fmin + i.
    void set_trim_tables(unsigned fmin,
                         const std::vector<cl_uint4>& p3f,
                         const std::vector<cl_uint4>& fcol);
    /// The candidate-id cap of every cpi route, after the device-memory budget.
    [[nodiscard]] std::size_t cand_id_cap() const;
    /// Device bytes per candidate-id slot (the u32 id list).
    [[nodiscard]] std::size_t cand_id_bytes() const noexcept;

    /// Creates the producer-pack pool before the producers start; false if none.
    bool init_pack_pool(
        bool trim_tables, unsigned fmin, std::size_t fcount, std::size_t max_n, std::size_t blocks);
    /// Producer thread (thread-safe): packs `batch` into a free block; false if none.
    bool pack_on_producer(SurvivorBatch& batch);
    /// Dispatch thread, for a finished `batch`: frees its unused block, reclaims completed ones.
    void retire_packed(SurvivorBatch& batch);
    [[nodiscard]] PackPoolStats pack_pool_stats() const;
    /// Runs one batch through survivor_tail16, one work-item per survivor.
    void run(const SurvivorBatch& batch, const SurvivorConst& c, SurvivorOut& out);
    /// Candidate-per-item dispatch: the fused device mask and scatter, then the tail.
    void run_cpi(const SurvivorBatch& batch,
                 const SurvivorConst& c,
                 collatz::u128 rlb,
                 std::uint32_t flag_cap,
                 std::size_t wg,
                 CpiResult& out);
    /// Two-deep run_cpi pipeline: enqueues `batch`, returns its slot or -1 (then use run_cpi).
    [[nodiscard]] int cpi_submit(const SurvivorBatch& batch,
                                 const SurvivorConst& c,
                                 collatz::u128 rlb,
                                 std::uint32_t flag_cap,
                                 std::size_t wg);
    /// Waits for `slot`, then validates and reduces its batch as run_cpi does.
    void cpi_complete(int slot, CpiResult& out);
    /// Waits for `slot` and drops its result unvalidated (self-check injection only).
    void cpi_discard(int slot);

private:
    SurvivorRunner() = default;
    struct Setup; // the steps of create() (survivor_runtime/setup.hpp)
    void ensure_capacity(std::size_t n);
    void release_io_buffers() noexcept;
    void ensure_cpi_capacity(std::size_t cands, std::size_t groups);
    void release_cpi_buffers() noexcept;
    struct DevCount;
    [[nodiscard]] std::size_t persist_groups() const noexcept;
    void cpi_pass2_persistent(std::size_t C,
                              const SurvivorConst& c,
                              collatz::u128 rlb,
                              std::uint32_t flag_cap,
                              std::size_t wg,
                              CpiResult& out,
                              DevCount& dc);
    struct PersistBufs;
    void persist_enqueue(const SurvivorConst& c,
                         collatz::u128 rlb,
                         std::uint32_t flag_cap,
                         std::size_t wg,
                         std::size_t groups,
                         const PersistBufs& b,
                         cl_uint nonce,
                         cl_uint dev_cap,
                         bool skip_launch);
    bool prefetch_inputs_bad(CpiResult& out);
    bool cpi_stage_inputs(const SurvivorBatch& batch, cl_uint& bits, cl_event& pf_wait);
    void cpi_enqueue_input_checks(const SurvivorBatch& batch,
                                  cl_uint bits,
                                  unsigned char (*dst)[16],
                                  unsigned char (*want)[16],
                                  std::size_t* len);
    void cpi_enqueue_mask(const SurvivorBatch& batch,
                          const SurvivorConst& c,
                          cl_uint nonce,
                          std::size_t cand_cap,
                          cl_uint bits,
                          cl_event pf_wait,
                          cl_mem cnt,
                          cl_mem ovf,
                          cl_mem sc);
    bool sc_check_mask(cl_uint nonce,
                       cl_uint cand_total,
                       cl_uint cand_ovf,
                       const cl_uint* scw,
                       std::uint64_t cupper,
                       std::size_t cand_cap,
                       cl_uint expect_groups, // survivors the mask groups must report
                       CpiResult& out);
    cl_uint sc_begin() noexcept;
    void sc_check_tail(cl_uint nonce,
                       std::size_t C,
                       std::size_t groups,
                       const std::vector<cl_ulong>& gcov,
                       const std::vector<cl_ulong>& gdig,
                       const std::vector<cl_ulong>& gblk,
                       const std::vector<cl_uint>& gcnt,
                       const std::vector<cl_uint4>& gmax,
                       const std::vector<cl_ulong>& gchk,
                       CpiResult& out) const;
    void arm_dropwg_hook(cl_command_queue q, cl_mem sc);
    struct PackedBlock;
    [[nodiscard]] bool take_packed(const SurvivorBatch& batch, cl_uint bits_try, PackedBlock& t);
    [[nodiscard]] const StagedWrite* packed_writes(int block) const noexcept;
    void set_packed_event(int block, cl_event ev) noexcept;
    void reclaim_packed();
    void release_pack_pool() noexcept;
    void infl_ensure(InflightSlot& s, std::size_t groups, std::size_t flag_cap);
    void infl_enqueue_readbacks(InflightSlot& s, std::size_t groups);
    void infl_release() noexcept;
    [[nodiscard]] cl_uint trim_bits_for(const SurvivorBatch& batch) const;
    void upload_inputs(cl_command_queue q,
                       const InputSet& set,
                       const SurvivorBatch& batch,
                       cl_uint& bits,
                       cl_event* ev,
                       bool skip_n0);
    [[nodiscard]] StagingSlot& next_staging(std::size_t bytes);
    // The staged pack, its slot size and the pre-pack match test.
    int pack_staged(const SurvivorBatch& batch, cl_uint& bits, unsigned char* base, StagedWrite* w);
    [[nodiscard]] static int pack_staged_core(const SurvivorBatch& batch,
                                              cl_uint& bits,
                                              unsigned char* base,
                                              std::size_t cap,
                                              StagedWrite* w,
                                              cl_uint fmin,
                                              std::size_t fcount);
    [[nodiscard]] static std::size_t staged_full_bytes(std::size_t n) noexcept;
    [[nodiscard]] bool prepack_matches(const SurvivorBatch& batch) const noexcept;
    [[nodiscard]] bool prefetch_matches(const SurvivorBatch& batch) const noexcept;
    void ensure_alt_inputs(std::size_t n);
    [[nodiscard]] InputSet active_inputs() const noexcept;
    void set_active_inputs(const InputSet& s) noexcept;
    void release_transfer() noexcept;
    void bind_trim_args(cl_kernel k, cl_uint& arg, cl_uint bits, bool mask_table);
};

} // namespace collatz::gpu

#endif // COLLATZ_GPU_SURVIVOR_RUNTIME_RUNNER_HPP
