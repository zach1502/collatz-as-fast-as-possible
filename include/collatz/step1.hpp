#ifndef COLLATZ_STEP1_HPP
#define COLLATZ_STEP1_HPP

// Angeltveit-style "Step 1" residue-class enumeration for the Collatz verifier.
//
// Step 1 sieves the residue classes n0 (mod 2^M) using only the low M bits: it
// prunes every class that provably converges whatever its high bits are. The
// survivors are the classes that Steps 2 and 3 still check.
//
// Method. The accelerated map is T(n) = n/2 for even n, (3n+1)/2 for odd n.
// The whole construction rests on one affine lemma:
//
//     T^k(n0 + a*2^k) = T^k(n0) + 3^f * a          (a >= 0)
//
// where f is the number of odd steps among the first k steps of n0 -- and f,
// like the parity of every one of those k steps, depends only on the low k
// bits (i.e. on n0). The classes are enumerated by a DFS that adds one more
// most-significant bit at each level. A node is the tuple (n0, m, k, f) with
// m = T^k(n0) for the class representative n0 in [0, 2^k):
//
//   * root = (3, 8, 2, 2): classes 0 (mod 2) [every even n >= 2 halves] and
//     1 (mod 4) [T^2(n) = (3n+1)/4 < n for n >= 5; n = 1 is the trivial cycle]
//     are excluded at depth <= 2 by convention, leaving only 3 (mod 4).
//   * children of (n0, m, k, f): let v be the depth-k value of the child's
//     representative -- v = m for the low child (new bit 0), v = m + 3^f for
//     the high child (new bit 1, representative n0 + 2^k). Then the child is
//     (child_n0, T(v), k+1, f + [v odd]). The two children always split into
//     one odd-step and one even-step child.
//
// Each node with k >= 2 goes through three sieves, in this order, before it is
// expanded (then the optional mod-3^t sieve, see Options::mod3t). A hit
// excludes the entire class n0 (mod 2^k) and prunes the subtree:
//
//   1. Descent:      m < n0. Then T^k dips below the start for the whole class
//                    (affine lemma + monotonicity), so every member converges.
//   2. Path-Merging: m == 2 (mod 3) and (2m-1)/3 < n0. Then n0's path joins the
//                    path of the strictly smaller (2m-1)/3 = T^{-1}-preimage.
//   3. Odd-Even-Even (Hercher): if at some depth j the value m_j = T^j(n0) is
//                    odd with (m_j-1)/2 < n0, and the steps from depth j are
//                    l >= 1 odd steps then exactly 2 even steps, then n0's path
//                    joins the path of the smaller (m_j-1)/2 at depth j+l+2.
//                    Tracked by a watch carried down the recursion.
//
// A node reaching depth k == M without a hit is a survivor.
//
// Overflow: n0 < 2^k gives m = T^k(n0) < 2*3^f <= 2*3^k (see kU64MaxDepth). The
// enumerators assert M <= 76, so every DFS value, including 3v+1 for
// v = m + 3^f, stays below 3^78 < 2^124 and u128 does not overflow; the
// mod-3^t shift m << L has its own per-node assert. MSVC is unsupported (u128).

#include "collatz/u128.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace collatz::preimage {
class ChainTable; // mod-3^t preimage chains (see preimage_chain.hpp).
} // namespace collatz::preimage

namespace collatz::step1 {

/// The partial-OEE threshold a* that the DFS carries in Node::oee_astar, so Step
/// 2 needs no walk of a survivor's M-step prefix. For a class n0 (mod 2^M) the
/// parities of steps 0..M-1 are those of n0, so the affine lemma gives every
/// extension's iterate: T^j(n0 + a*2^M) = m_j + 3^{f_j} 2^{M-j} a, m_j = T^j(n0).
/// An arm is a depth j with m_j odd, followed by l >= 1 odd steps and 2 even
/// steps, the last at a step index <= M-1. By Hercher's Lemma 9 the path of
/// n = n0 + a*2^M then joins that of (T^j(n) - 1)/2, a smaller number when
/// a*C > D, with C = 2^{M-j}(2^{j+1} - 3^{f_j}) and D = m_j - 1 - 2 n0. For
/// C > 0 that holds for every a >= a*(j) = floor(D/C) + 1 when D >= 0 (at
/// D == 0 the target is n0 itself, so a = 0 stays) and for every a when D < 0
/// (a*(j) = 0). `oee_astar` is the min of a*(j) over the class's arms, evaluated
/// at the full leaf n0, and Step 2 keeps the candidates a < oee_astar; arms that
/// complete past depth M are left to the Step-2 window. kOeeAStarNone (no arm
/// with C > 0, or tracking off) compares >= 2^A for every A <= step2::max_A, so
/// mask() treats it as "no prune".
inline constexpr u128 kOeeAStarNone = u128_max;

/// Largest emission depth M enumerated in u64. At depth k <= M with f odd
/// steps, 2^k T^k(n0) = 3^f n0 + c_k, c_k < 3^f 2^k, n0 < 2^k, so m < 2*3^f;
/// every DFS quantity (2m, m + 3^f, (3v+1)/2, pmax, smax) is then below
/// 4*3^M < 2^64 (the mod-3^t `m << L` is guarded per node).
inline constexpr unsigned kU64MaxDepth = 39;

/// A surviving residue class: representative n0 (mod 2^M), m = T^M(n0), and f =
/// the number of odd steps among those M steps.
///
/// Prefix bound (pmax, smax). For a candidate n = n0 + a*2^M (a >= 0) the affine
/// lemma gives T^j(n) = T^j(n0) + 3^{f_j} 2^{M-j} a = b_j + s_j a for j in
/// [0, M], so the candidate's exact prefix maximum is <= pmax + smax*a where
/// pmax >= max_j b_j and smax = max_j s_j. Both are carried incrementally down
/// the DFS (no per-survivor prefix re-walk): each node at depth k contributes
/// its slope s_k = 3^{f_k} 2^{M-k} to smax and its intercept upper bound
/// (m_k + s_k) to pmax, where m_k = T^k(n0 mod 2^k) is the node value. Since the
/// full survivor's b_k = T^k(n0) = m_k + 3^{f_k}(n0 >> k) and (n0 >> k) < 2^{M-k},
/// b_k < m_k + s_k, so pmax = max_k(m_k + s_k) is a valid (slightly loose) upper
/// bound on max_k b_k. The reconstructed global maximum stays exact without the
/// M-step per-survivor walk: the bound only over-estimates, so a record
/// contender is never skipped. smax is exact.
struct Node {
    u128 n0;
    u128 m;
    unsigned f;
    u128 pmax = 0; ///< Upper bound on max_{0<=j<=M} T^j(n0) (see above).
    u128 smax = 0; ///< max_{0<=j<=M} 3^{f_j} 2^{M-j} (exact slope bound).
    /// Partial-OEE threshold a* carried down the DFS (see kOeeAStarNone
    /// above). kOeeAStarNone unless Options::track_oee_astar is set.
    u128 oee_astar = kOeeAStarNone;
};

/// Reason codes reported through Options::on_exclude.
enum Reason : int {
    reason_descent = 1,    ///< Descent sieve: m < n0.
    reason_path_merge = 2, ///< Path-Merging sieve: m == 2 (mod 3), (2m-1)/3 < n0.
    reason_oee = 3,        ///< Odd-Even-Even sieve.
    reason_mod3t = 4,      ///< Generalized mod-3^t preimage-chain merge.
};

/// Enumeration options plus an optional per-exclusion callback. The callback
/// (when set) is invoked once per excluded class with the class representative
/// n0, the depth k at which it was excluded (so the class is n0 mod 2^k), and the
/// Reason. Tests use it to validate every exclusion; the enumeration does not
/// need it.
struct Options {
    /// Generalized mod-3^t preimage-chain merge sieve. 0 disables it; t >= 1
    /// enables an extra sieve that, after Descent / Path-Merging / OEE,
    /// excludes a class n0 (mod 2^k) when
    /// m = T^k(n0) admits a backward Collatz chain (keyed on m mod 3^t) landing
    /// on a strictly smaller integer for every member of the class. The sound
    /// rule requires, for the chosen chain with L total / v odd backward steps
    /// and constant K (A(x) = (2^L x - K)/3^v; see collatz/preimage_chain.hpp):
    ///   (i)   f >= v            -- residue uniformity across all extensions a,
    ///   (ii)  A(m) < n0         -- base merge target is smaller,
    ///   (iii) 2^L*3^{f-v} <= 2^k -- ancestor stays below n for all extensions.
    /// t == 1 reproduces Path-Merging exactly, which is redundant but harmless.
    /// Values beyond preimage::max_t are rejected by the enumerator.
    unsigned mod3t = 0;
    /// When true, the DFS records each completing OEE arm (run-start value
    /// m_j = T^j(n0), 3^{f_j} multiplier, depth j) and computes the partial-OEE
    /// threshold a* at each survivor's full leaf n0, storing it in
    /// Node::oee_astar (see kOeeAStarNone). Off by default: it adds a small
    /// per-path arm stack, so other Step-1 callers (which never read oee_astar)
    /// do not pay for it. test_core_step1_dfs checks the value against a walk of
    /// each survivor's M-step prefix.
    bool track_oee_astar = false;
    /// Case splitting (Angeltveit-style slice enumeration). Run only slice
    /// `case_c` of `case_k` equal subtrees, sliced by the low bits of n0.
    /// `case_k` must be a power of two; each slice is the DFS subtree of the
    /// depth-(2+log2 case_k) node whose n0 bits [2, 2+log2 case_k) equal
    /// `case_c` (the low two bits are always 3, since only n0 == 3 (mod 4)
    /// survive the root). `case_c` in [0, case_k). `case_k == 1` (the default)
    /// runs the whole range. The enumerator descends only the matching child
    /// from the root down to the slice depth (no sibling / upper-tree work) and
    /// then explores that subtree, so a slice costs only its own subtree. The
    /// disjoint union over `case_c` in [0, case_k) of the slices is exactly the
    /// full (case_k == 1) run: candidates, coverage, pow digest and the
    /// max-excursion reduction all partition.
    std::uint64_t case_k = 1;
    std::uint64_t case_c = 0;
    /// Force the u128 DFS even when M <= kU64MaxDepth (tests compare the two;
    /// the emitted Node set is identical).
    bool force_u128 = false;
    /// Optional prebuilt mod-3^t table, used when its t() equals mod3t; the
    /// enumerators otherwise build one per call. A caller that enumerates
    /// many single-path slices (certify's audit) passes one to skip the builds.
    const preimage::ChainTable* chain_table = nullptr;
    /// Optional. Called synchronously from the enumerating thread(s); under
    /// enumerate_parallel it may be called concurrently.
    std::function<void(u128 n0, unsigned depth, int reason)> on_exclude;
};

/// Survivor count and wall-clock time of one enumeration.
struct Stats {
    std::uint64_t survivors = 0; ///< Classes reaching depth M uncut.
    double seconds = 0.0;        ///< Wall-clock time of the enumeration.
};

/// Enumerates all surviving residue classes mod 2^M on one thread and calls
/// `sink` once per survivor, in DFS order. Returns the aggregate Stats.
/// For M < 2 there is nothing to enumerate and an empty Stats is returned.
Stats enumerate(unsigned M, const Options& opts, const std::function<void(const Node&)>& sink);

/// Parallel variant of enumerate. The DFS is expanded serially down to a fixed
/// split depth; each not-yet-pruned node at that depth becomes an independent
/// subtree task, and the tasks are processed by `threads` worker threads.
///
/// Concurrency: `sink` and Options::on_exclude may be invoked concurrently from
/// multiple worker threads. Callers must synchronize any shared state they
/// touch. The returned Stats is the merged total. With threads <= 1, or M at or
/// below the split depth, this falls back to the serial enumerate.
Stats enumerate_parallel(unsigned M,
                         unsigned threads,
                         const Options& opts,
                         const std::function<void(const Node&)>& sink);

/// Batch sink for enumerate_parallel_buffered: invoked once per block of
/// survivors produced by worker `tid`, with a pointer to `count` contiguous
/// Nodes (owned by the worker's private buffer, valid only for the duration of
/// the call). One call per batch of survivors, instead of a std::function call
/// per survivor, hands the consumer a contiguous run it can process without
/// re-entering the DFS. `tid` (0 .. threads-1) is stable for the lifetime of the
/// run, and distinct tids may run concurrently, so key any per-thread state by
/// `tid` and merge after.
using BatchSink = std::function<void(unsigned tid, const Node* nodes, std::size_t count)>;

/// Parallel enumerate that streams survivors into per-thread buffers and drains
/// them in blocks of at most 4096 Nodes through `consume_batch` (see
/// BatchSink). No std::function is invoked per survivor; the DFS appends into a
/// worker-private std::vector<Node> and flushes a full batch (and any remainder
/// at the end) via one BatchSink call. Semantically identical to
/// enumerate_parallel: the multiset of survivors delivered is the same, only the
/// delivery is batched. With threads <= 1 (or a tree at/under the split depth)
/// it runs serially, buffering into a single tid-0 batch.
///
/// `on_worker_done` (optional) is invoked once per worker `tid`, on that
/// worker's thread, right after its last consume_batch call (the serial path
/// calls it once with tid 0). Distinct tids may run it concurrently, under the
/// same per-tid contract as consume_batch. A tid that never ran (more threads
/// than tasks) gets no call, so the caller must still handle any state left
/// after the function returns. The serial path also skips the call when
/// `M < root_k` (the tree is empty: no batches and no hook).
///
/// `on_worker_done` must not throw: on the threaded path it runs inside a
/// std::thread body, where an escaping exception calls std::terminate instead
/// of reaching the caller (only the serial path would propagate it).
using WorkerDoneHook = std::function<void(unsigned tid)>;
Stats enumerate_parallel_buffered(unsigned M,
                                  unsigned threads,
                                  const Options& opts,
                                  const BatchSink& consume_batch,
                                  const WorkerDoneHook& on_worker_done = {});

} // namespace collatz::step1

#endif // COLLATZ_STEP1_HPP
