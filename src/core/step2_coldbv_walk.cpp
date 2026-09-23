// Step 2 class-major dip walk: fills the class-major dip table, one prefix class
// per row, with a binary-tree walk of the last H steps.

#include "collatz/pow3.hpp"
#include "collatz/u128.hpp"
#include "step2_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <memory>

namespace collatz::step2 {

using namespace detail;

namespace {

/// The dip walk state of one residue prefix, with every field 32/64-bit (no
/// byte-wide stores when a tree node is copied), plus v mod 3^tc advanced
/// without division: v/2 == half(v) and (3v+1)/2 == half(3v+1) mod an odd
/// modulus, where half(x) = (x + (x&1) mod) / 2.
struct DipWalk {
    std::uint64_t v;    ///< T^k of the node's residue (exact, < 3^B).
    std::int32_t best;  ///< max over every term decided so far.
    std::int32_t a;     ///< A_k = 306 k - 485 f_k.
    std::int32_t oee;   ///< A at the start of the current odd run (mode 1/2).
    std::int32_t last;  ///< index of the last odd step, -1 if none.
    std::uint32_t f;    ///< f_k.
    std::uint32_t mode; ///< 0 idle, 1 in odd run, 2 run ended by one even.
    std::uint32_t vm;   ///< v mod g.mod.
};

/// Flat view of DipGain for the hot loop (locals, so nothing is reloaded
/// through the struct after each store). `off[c]` is the row offset.
struct GainView {
    const std::uint16_t* off;
    const std::uint16_t* rows;
    std::uint32_t mod;
    std::uint32_t tc;
    bool on;
};

inline void dip_walk_terms(DipWalk& p, unsigned k, const GainView g) noexcept {
    // Terms at window step k, as in Filter::dip's k-loop body (the OEE term is
    // admitted in dip_walk_step), with the two window lookups fused into one (a
    // max, so order-free). best >= a after the first line, so a + 0 never raises
    // it and the g > 0 test is implicit.
    p.best = std::max(p.best, p.a);
    if (p.f >= 1u) {
        const unsigned trailing = (k - 1u) - static_cast<unsigned>(p.last);
        const std::int32_t pm = p.a + 179;
        p.best = ((trailing & 1u) == 0u && pm > p.best) ? pm : p.best;
        if (g.on) {
            const unsigned o = std::min<unsigned>(p.f, g.tc);
            p.best = std::max(p.best, p.a + static_cast<std::int32_t>(g.rows[g.off[p.vm] + o]));
        }
    }
}

inline void dip_walk_step(DipWalk& p, unsigned s, std::uint32_t mod) noexcept {
    // Advances from depth s to s+1 without branches: the parity of a fresh
    // residue is a coin flip, so a branch would mispredict about every other
    // step.
    const std::uint64_t odd = p.v & 1u;
    const bool o = odd != 0u;
    std::uint32_t y = o ? 3u * p.vm + 1u : p.vm; // < 3 mod + 1
    y -= y >= mod ? mod : 0u;
    y -= y >= mod ? mod : 0u;
    y -= y >= mod ? mod : 0u;
    p.vm = (y + ((y & 1u) != 0u ? mod : 0u)) >> 1u;
    // OEE run automaton: an odd run starting at step k0 and followed by two even
    // steps (both < B) admits max_{k in run} (A_k + 306) = A_{k0} + 306, since A
    // falls inside a run. Odd opens a run (recording A), one even ends it
    // (mode 2), a second even admits A_run + 306.
    const bool m1 = p.mode == 1u;
    const bool m2 = p.mode == 2u;
    const std::int32_t oee_term = p.oee + static_cast<std::int32_t>(kHalveWeight);
    p.best = (!o && m2 && oee_term > p.best) ? oee_term : p.best;
    p.oee = (o && !m1) ? p.a : p.oee;
    p.mode = o ? 1u : (m1 ? 2u : 0u);
    p.last = o ? static_cast<std::int32_t>(s) : p.last;
    p.f += static_cast<std::uint32_t>(odd);
    p.a += o ? static_cast<std::int32_t>(kHalveWeight - kOddWeight)
             : static_cast<std::int32_t>(kHalveWeight);
    p.v = (p.v + ((2u * p.v + 1u) & (std::uint64_t{0} - odd))) >> 1u; // (3v+1)/2 or v/2.
}

} // namespace

namespace detail {

// Fills cm with the dip of every residue in class-major order: row c holds
// the 2^H residues c + 2^L h, L = B - H.
void walk_dip_classmajor(const std::unique_ptr<std::uint16_t[]>& cm,
                         const DipGain& g,
                         unsigned B,
                         unsigned H,
                         unsigned nthreads) {
    const unsigned L = B - H;
    const std::uint64_t classes = std::uint64_t{1} << L;
    const std::uint64_t hspan = std::uint64_t{1} << H;
    const std::uint32_t mod = g.mod;
    std::uint32_t p3mod[64];
    for (unsigned f = 0; f < 64u; ++f) {
        p3mod[f] = static_cast<std::uint32_t>(pow3(f) % mod); // 3^f mod 3^tc.
    }
    const GainView gv{g.id.data(), g.rows.data(), g.mod, g.tc, g.on};
    std::uint64_t p3lo[64];
    for (unsigned f = 0; f < 64u; ++f) {
        p3lo[f] = lo64(pow3(f)); // exact for f <= B <= 32.
    }
    run_chunked(nthreads, classes, 1024, [&](std::uint64_t lo, std::uint64_t hi) {
        // The child of `par` (depth k) whose bit k of r is b: v gains 3^{f_k} b
        // (T^k(r' + 2^k b) = T^k(r') + 3^{f_k} b), then takes one step and its
        // terms.
        const auto child = [&](const DipWalk& par, std::uint32_t b, unsigned k) {
            DipWalk ch = par;
            if (b != 0u) {
                ch.v += p3lo[ch.f];
                ch.vm += p3mod[ch.f];
                ch.vm -= ch.vm >= mod ? mod : 0u;
            }
            dip_walk_step(ch, k, mod);
            dip_walk_terms(ch, k + 1u, gv);
            return ch;
        };
        const auto leaf = [](const DipWalk& st) {
            return static_cast<std::uint16_t>(st.best < 0 ? 0 : st.best);
        };
        for (std::uint64_t c = lo; c < hi; ++c) {
            DipWalk pre{c, 0, 0, 0, -1, 0, 0, static_cast<std::uint32_t>(c % mod)};
            for (unsigned s = 0; s < L; ++s) {
                dip_walk_terms(pre, s, gv);
                dip_walk_step(pre, s, mod);
            }
            std::uint16_t* row = cm.get() + (c << H);
            // Binary-tree walk of the last H levels. A node at depth k
            // stands for r mod 2^k with v = T^k(r mod 2^k), its terms
            // already applied (they depend on r mod 2^k alone), so a
            // leaf costs about 2 steps instead of H. stk[j] is the node at
            // depth L + j on the current path; pairs of sibling leaves
            // (h and h + 2^{H-1}) are finished straight from stk[H-1].
            DipWalk stk[13];
            std::uint32_t hs[13];
            dip_walk_terms(pre, L, gv);
            stk[0] = pre;
            hs[0] = 0;
            const unsigned top = H - 1u; // levels 0..top-1 on the stack.
            for (std::uint32_t q = 0; q < (hspan >> 1u); ++q) {
                // q's bits, MSB first, pick the child at levels 0..top-1;
                // only the levels below q's lowest changed bit are redone.
                const unsigned j0 =
                    q == 0 ? 0u : top - 1u - static_cast<unsigned>(std::countr_zero(q));
                for (unsigned j = j0; j < top; ++j) {
                    const std::uint32_t b = (q >> (top - 1u - j)) & 1u;
                    stk[j + 1u] = child(stk[j], b, L + j);
                    hs[j + 1u] = hs[j] | (b << j);
                }
                const DipWalk& par = stk[top];
                row[hs[top]] = leaf(child(par, 0u, L + top));
                row[hs[top] | (1u << top)] = leaf(child(par, 1u, L + top));
            }
        }
    });
}

} // namespace detail

} // namespace collatz::step2
