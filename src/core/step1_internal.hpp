#pragma once

// Private declarations shared by the Step 1 sources: the per-path DFS state, the
// enumeration context Ctx, the node classes and the functions the enumerators call.

#include "collatz/pow3.hpp"
#include "collatz/step1.hpp"
#include "collatz/u128.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace collatz::step1::detail {

/// The prefix bound carried down the DFS (see step1::Node). `pmax` is an upper
/// bound on max_{0<=j<=M} T^j(n0), and `smax` is max_{0<=j<=M} of the slope
/// 3^{f_j} 2^{M-j}. Each node adds one max to each, so a survivor has its whole
/// prefix bound without walking the prefix again.
template <class V>
struct PrefixCarryT {
    V pmax = 0;
    V smax = 0;
};
using PrefixCarry = PrefixCarryT<u128>;

/// State of the odd-even-even watch carried down the recursion.
///
/// The watch detects the pattern "an odd value m_j whose forward trajectory is
/// l >= 1 odd steps then 2 even steps". It arms at the start of an odd run: m
/// increases along an odd run, so its first value is the smallest and the best
/// candidate for (m_j-1)/2 < n0. `mj` equals T^j(n0) for the current class
/// representative: adding a high bit at frontier position i adds 3^{fj} * 2^{i-j}
/// to T^j (affine lemma), so `mj` grows by that amount whenever a high child is
/// taken while armed.
enum class Phase : std::uint8_t {
    none,    ///< Not tracking; a following odd step arms a run.
    odd_run, ///< Inside the l>=1 odd steps.
    even1,   ///< Odd run ended; one even step seen, need one more.
    done,    ///< Odd run + 2 evens complete at this node: the OEE test applies.
};

template <class V>
struct WatchT {
    Phase phase = Phase::none;
    V mj = 0;        ///< T^j(n0) for the current representative (when armed).
    unsigned fj = 0; ///< f at the arm depth j (the 3^{fj} multiplier).
    unsigned j = 0;  ///< Arm depth.
};
using Watch = WatchT<u128>;

/// One completing OEE arm on the current DFS path, for the partial-OEE a*. `mj`
/// and `n0` are taken at the node where the arm completes (mj = T^j(rep), n0 =
/// that class representative). The leaf recovers m_j = T^j(full n0) by the affine
/// lemma m_j^{(leaf)} = mj + 3^{fj} * ((n0_leaf - n0) >> j); the bits added after
/// the arm completes lie at positions >= j+l+2 > j, so the shift is exact. `j` and
/// `fj` are the arm depth and its 3^{fj} multiplier.
template <class V>
struct OeeArmT {
    V mj;
    V n0;
    unsigned j;
    unsigned fj;
};
using OeeArm = OeeArmT<u128>;

/// Capacity of the per-path stack of completing OEE arms. An arm is recorded at
/// a node whose arm watch is `done` (an odd run, then 2 even steps), so the arms
/// on one path lie at depths >= 4, at least 3 apart: at most (M - 4) / 3 + 1,
/// which is 25 at M = 76, the largest depth the enumerators accept.
inline constexpr unsigned kArmCap = 64;
static_assert((76 - 4) / 3 + 1 <= kArmCap, "an M <= 76 path records at most 25 arms");
/// Capacity of the arm snapshot in a Frame. The prelude defers a node at depth
/// kSplitDepth before recording its arm, so a Frame holds the arms of depths 4 ..
/// kSplitDepth - 1 (static_assert below), and a small array keeps Frame small.
inline constexpr unsigned kFrameArmCap = 16;

template <class V>
struct ArmStackT {
    std::array<OeeArmT<V>, kArmCap> a{};
    unsigned n = 0;
};
using ArmStack = ArmStackT<u128>;

/// A deferred subtree root for the parallel enumerator.
struct Frame {
    u128 n0;
    u128 m;
    unsigned k;
    unsigned f;
    Watch watch;
    PrefixCarry carry;
    // The partial-OEE arm watch, which the worker continues. It is seeded in
    // the initial odd run at j = 0, while the sieve `watch` starts empty at
    // depth 2. Used only under track_oee_astar.
    Watch awatch;
    // The completing OEE arms on the path down to this node, so the worker
    // continues the DFS with the same arm prefix.
    std::array<OeeArm, kFrameArmCap> arms{};
    unsigned narms = 0;
};

/// Worker-private survivor buffer: `n` filled slots of a `cap`-sized block.
struct EmitBuf {
    std::vector<Node> v;
    std::size_t n = 0;
    explicit EmitBuf(std::size_t cap) : v(cap) {}
};

/// Packed mod-3^t chain entry for the u64 DFS; v = kNoChainV when there is none.
struct FlatChain {
    std::uint64_t K;
    std::uint32_t p3v; ///< 3^v
    std::uint8_t L;
    std::uint8_t v;
};
inline constexpr std::uint8_t kNoChainV = 0xff;

/// kNeedBits[d] = the least s with 3^d <= 2^s (d <= kU64MaxDepth).
inline constexpr auto kNeedBits = [] {
    std::array<std::uint8_t, kU64MaxDepth + 1> a{};
    for (unsigned d = 0; d <= kU64MaxDepth; ++d) {
        a[d] = static_cast<std::uint8_t>(bit_length(POW3[d] - 1));
    }
    return a;
}();

/// Shared, read-only context for one enumeration.
struct Ctx {
    unsigned M;
    const Options* opts;
    const std::function<void(const Node&)>* sink = nullptr; ///< Per-survivor sink (unbuffered).
    unsigned split_depth = 0;                    ///< Nodes at this depth are deferred (parallel).
    std::vector<Frame>* tasks = nullptr;         ///< Receives the deferred frames when set.
    const preimage::ChainTable* chain = nullptr; ///< mod-3^t table (null when off).
    // Buffered-emit path (enumerate_parallel_buffered): when `buffer` is set the
    // survivor is appended here instead of invoking `sink`, and a full batch is
    // flushed through `batch_sink`, so there is no std::function call per survivor.
    EmitBuf* buffer = nullptr;
    const BatchSink* batch_sink = nullptr;
    unsigned tid = 0;
    std::size_t batch_cap = 0;
    /// Case splitting (Options::case_c of case_k): when case_depth != 0 the DFS
    /// takes only the child matching bit k of `case_low` at every depth k in
    /// [2, case_depth), and both children below. case_low holds the target n0's
    /// low case_depth bits.
    unsigned case_depth = 0;
    std::uint64_t case_low = 0;
    bool u64 = false; ///< Run the DFS in std::uint64_t (see kU64MaxDepth).
    // NOLINTNEXTLINE(readability-redundant-member-init): {} quiets -Wmissing-field-initializers
    std::vector<FlatChain> flat{}; ///< Packed copy of `chain` (u64 DFS only).
    std::uint64_t flat_mod = 1;
};

/// The root node (3, 8, 2, 2). For M < 2 there is nothing to enumerate.
constexpr u128 root_n0 = 3;
constexpr u128 root_m = 8;
constexpr unsigned root_k = 2;
constexpr unsigned root_f = 2;

/// The DFS depth at which the serial prelude stops and hands each surviving
/// subtree to the worker pool as a task. A deeper split gives more, smaller tasks
/// and finer load balancing (a shorter tail at the end of the run) for a slightly
/// longer prelude; the prelude does no Step-2/3 work, so it stays cheap until the
/// per-task overhead dominates. 18 gives about 5.3k tasks at N=40.
constexpr unsigned kSplitDepth = 18;
static_assert((kSplitDepth - 1 - 4) / 3 + 1 <= kFrameArmCap,
              "a prelude path records at most 5 arms before it is deferred");

// Defined in step1_setup.cpp.
void case_params(const Options& opts, unsigned& case_depth, std::uint64_t& case_low);
void set_dfs_width(Ctx& ctx, const Options& opts, unsigned M);
void merge_stats(Stats& into, const Stats& from);
const preimage::ChainTable* chain_table_for(const Options& opts,
                                            std::unique_ptr<preimage::ChainTable>& own);

// Defined in step1_dfs.cpp.
void dfs_root(const Ctx& ctx, Stats& st);
void dfs_task(const Ctx& ctx, Stats& st, const Frame& fr);

} // namespace collatz::step1::detail
