#ifndef COLLATZ_TESTS_FRONTIER_MODEL_SHIPPED_HPP
#define COLLATZ_TESTS_FRONTIER_MODEL_SHIPPED_HPP

// The shipped configuration against the plain block model: the pipeline's
// candidate set (pipeline_candidates) and an independent GMP check of each
// exclusion (replay_exclusion). frontier_model.hpp includes this file.

#include "collatz/frontier.hpp"
#include "collatz/oracle.hpp"
#include "collatz/step1.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"
#include "collatz/verify.hpp"
#include "frontier_model/block_model.hpp"
#include "frontier_model/walk.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <doctest/doctest.h>
#include <gmpxx.h>
#include <iterator>
#include <vector>

namespace collatz::frontier_test {

/// The candidate set the pipeline checks for the block run `o`, rebuilt from
/// the production calls verify::run makes per survivor: Step 1
/// (step1::enumerate), the survivor lift (frontier::lift_survivor) and
/// Filter::mask. Sorted ascending.
inline std::vector<u128> pipeline_candidates(const v::Options& o) {
    const unsigned M = o.N - o.A;
    collatz::step1::Options s1 = o.step1;
    s1.track_oee_astar = o.partial_oee;
    std::vector<collatz::step1::Node> surv;
    collatz::step1::enumerate(
        M, s1, [&surv](const collatz::step1::Node& node) { surv.push_back(node); });
    collatz::step2::Params p{o.N, o.A, o.B, o.num_bitvectors, 1u, o.partial_oee};
    p.window_chains = o.window_chains;
    p.window_templates_D = o.window_templates_D;
    p.use_bv_cache = o.bv_cache;
    const collatz::step2::Filter filter(p);
    const collatz::frontier::Lift lift = collatz::frontier::make_lift(o.block_h, o.N, o.A);
    std::vector<u128> out;
    for (collatz::step1::Node node : surv) {
        collatz::frontier::lift_survivor(node, lift);
        const collatz::step2::CandidateMask mask = filter.mask(node);
        for (unsigned a = 0; a < (1u << o.A); ++a) {
            if (mask.test(a)) {
                out.push_back(node.n0 + (static_cast<u128>(a) << M));
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

/// Backward preimage search from w (even preimage 2u, odd preimage (2u-1)/3
/// when u == 2 mod 3) for an ancestor 1 <= y < n within `depth` steps; on
/// success y and its distance L are stored.
inline bool ancestor_below(const mpz_class& w,
                           const mpz_class& n,
                           unsigned depth,
                           unsigned dist,
                           mpz_class& y,
                           unsigned& L) {
    if (depth == 0) {
        return false;
    }
    const mpz_class ue = 2 * w;
    if (ue < n) {
        y = ue;
        L = dist + 1;
        return true;
    }
    const bool odd = mpz_class(w % 3) == 2;
    const mpz_class uo = odd ? mpz_class((2 * w - 1) / 3) : mpz_class(0);
    if (odd && uo >= 1 && uo < n) {
        y = uo;
        L = dist + 1;
        return true;
    }
    return ancestor_below(ue, n, depth - 1, dist + 1, y, L) ||
           (odd && ancestor_below(uo, n, depth - 1, dist + 1, y, L));
}

/// An exclusion of n must reduce n below itself on its own trajectory within
/// `window` steps: a descent T^s(n) < n, or an ancestor 1 <= y < n of some
/// T^s(n) (backward search of depth <= `depth`) with T^L(y) == T^s(n), checked
/// by walking y forward. GMP throughout; independent of certify.cpp.
inline bool replay_exclusion(u128 n_u, unsigned window, unsigned depth) {
    const mpz_class n = collatz::oracle::to_mpz(n_u);
    std::vector<mpz_class> w(static_cast<std::size_t>(window) + 1);
    w[0] = n;
    for (unsigned s = 0; s < window; ++s) {
        w[s + 1] = T_mpz(w[s]);
        if (w[s + 1] < n) {
            return true; // descent
        }
    }
    for (unsigned s = 0; s <= window; ++s) {
        mpz_class y;
        unsigned L = 0;
        if (ancestor_below(w[s], n, depth, 0, y, L)) {
            mpz_class x = y;
            for (unsigned i = 0; i < L; ++i) {
                x = T_mpz(x);
            }
            if (x == w[s]) {
                return true; // merge: T^L(y) == T^s(n), y < n
            }
        }
    }
    return false;
}

/// The shipped configuration (window chains t_w = 6, templates D = 12) against
/// the plain model `md` of the same block (K, H, B): the shipped candidate set,
/// rebuilt from the production calls and matched to verify::run by count and
/// coverage digest, is a subset of the model's, and every shipped-only
/// exclusion passes replay_exclusion on the concrete n (window M + B, backward
/// depth max(2 t_w, D) = 12). Returns the number of shipped-only exclusions.
inline std::size_t check_shipped_vs_model(unsigned K, u128 H, unsigned B, const Model& md) {
    const v::Options so = shipped(K, H, B);
    const v::Result rs = v::run(so);
    const std::vector<u128> ship = pipeline_candidates(so);
    CHECK(ship.size() == rs.candidates_checked);
    u128 cov = 0;
    for (const u128 n : ship) {
        cov += cov_term(n);
    }
    CHECK(static_cast<std::uint64_t>(cov % kP) == rs.coverage_checksum);
    REQUIRE(md.cands.size() == md.candidates); // the model kept its list
    CHECK(std::includes(md.cands.begin(), md.cands.end(), ship.begin(), ship.end()));
    std::vector<u128> only;
    std::set_difference(
        md.cands.begin(), md.cands.end(), ship.begin(), ship.end(), std::back_inserter(only));
    const unsigned window = K - so.A + B;
    std::size_t bad = 0;
    for (const u128 n : only) {
        if (!replay_exclusion(n, window, 12)) {
            ++bad;
            if (bad <= 5) {
                MESSAGE(
                    "shipped-only exclusion without a certificate: n = " << collatz::to_string(n));
            }
        }
    }
    CHECK(bad == 0);
    MESSAGE("K=" << K << " H=" << collatz::to_string(H) << ": shipped " << ship.size()
                 << " of the plain model's " << md.cands.size() << " candidates; " << only.size()
                 << " shipped-only exclusions replayed");
    return only.size();
}

} // namespace collatz::frontier_test

#endif // COLLATZ_TESTS_FRONTIER_MODEL_SHIPPED_HPP
