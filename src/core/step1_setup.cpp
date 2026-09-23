// Step 1 enumeration setup shared by the enumerators: case slicing, the DFS width,
// the mod-3^t table and the Stats merge.

#include "collatz/pow3.hpp"
#include "collatz/preimage_chain.hpp"
#include "collatz/step1.hpp"
#include "step1_internal.hpp"

#include <cassert>
#include <cstdint>
#include <memory>

namespace collatz::step1::detail {

/// Resolves (case_c, case_k) into a slice depth and the target low-bit pattern
/// of n0. case_k <= 1 disables slicing (depth 0). case_k must be a power
/// of two and case_c < case_k (asserted); the slice is the subtree whose n0 bits
/// [2, 2+log2 case_k) equal case_c (low two bits fixed to 3).
void case_params(const Options& opts, unsigned& case_depth, std::uint64_t& case_low) {
    if (opts.case_k <= 1) {
        case_depth = 0;
        case_low = 0;
        return;
    }
    unsigned bits = 0;
    while ((static_cast<std::uint64_t>(1) << bits) < opts.case_k) {
        ++bits;
    }
    assert((static_cast<std::uint64_t>(1) << bits) == opts.case_k &&
           "case_k must be a power of two");
    assert(opts.case_c < opts.case_k && "case_c must be < case_k");
    case_depth = 2u + bits;
    case_low = (opts.case_c << 2u) | 3u;
}

/// Picks the u64 or u128 DFS for depth M and, for the u64 DFS, packs the
/// mod-3^t chain table. Every Ctx construction calls it with case_params.
void set_dfs_width(Ctx& ctx, const Options& opts, unsigned M) {
    ctx.u64 = !opts.force_u128 && M <= kU64MaxDepth;
    if (ctx.u64 && ctx.chain != nullptr && ctx.tasks == nullptr) { // prelude runs u128
        const preimage::ChainTable& tbl = *ctx.chain;
        ctx.flat_mod = static_cast<std::uint64_t>(tbl.modulus());
        ctx.flat.assign(static_cast<std::size_t>(ctx.flat_mod), FlatChain{0, 1, 0, kNoChainV});
        for (std::uint64_t r = 0; r < ctx.flat_mod; ++r) {
            const preimage::ChainEntry& e = tbl.entry(r);
            if (e.has_chain) {
                ctx.flat[r] = FlatChain{static_cast<std::uint64_t>(e.K),
                                        static_cast<std::uint32_t>(pow3(e.v)),
                                        static_cast<std::uint8_t>(e.L),
                                        static_cast<std::uint8_t>(e.v)};
            }
        }
    }
}

void merge_stats(Stats& into, const Stats& from) {
    into.survivors += from.survivors;
}

/// The mod-3^t table of opts.mod3t: opts.chain_table when it is one, else a
/// new one in `own` (none when mod3t is 0).
const preimage::ChainTable* chain_table_for(const Options& opts,
                                            std::unique_ptr<preimage::ChainTable>& own) {
    if (opts.mod3t == 0) {
        return nullptr;
    }
    if (opts.chain_table != nullptr && opts.chain_table->t() == opts.mod3t) {
        return opts.chain_table;
    }
    own = std::make_unique<preimage::ChainTable>(opts.mod3t);
    return own.get();
}

} // namespace collatz::step1::detail
