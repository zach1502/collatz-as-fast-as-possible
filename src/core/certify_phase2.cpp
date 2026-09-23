// Phase 2 of certify::run(): the re-derivation of every Step-2 extension
// exclusion of one Step-1 survivor; see certify.cpp.

#include "certify_internal.hpp"
#include "collatz/certify.hpp"
#include "collatz/frontier.hpp"
#include "collatz/oracle.hpp"
#include "collatz/step2.hpp"
#include "collatz/u128.hpp"

#include <cstdint>
#include <gmpxx.h>

namespace collatz::certify::detail {

// Phase 2: certify every Step-2 extension exclusion of one survivor.
void Auditor::survivor(const Node& node_in) {
    a_survivors.add(1);
    // Block mode: lift the survivor into the block (frontier.hpp), the one
    // transformation verify::run applies before Step 2, so the mask below
    // and every n audited are the block's own numbers.
    Node node = node_in;
    if (block) {
        frontier::lift_survivor(node, lift);
    }
    // Per-survivor locals, published once at the end of this call.
    std::uint64_t n_checked = 0, n_excluded = 0, n_mod9 = 0, n_flt = 0, n_window = 0;
    std::uint64_t n_replayed = 0, n_within = 0, n_inwin = 0, n_chain = 0;
    const auto publish = [&] {
        a_checked.add(n_checked);
        a_excluded.add(n_excluded);
        a_mod9.add(n_mod9);
        a_flt.add(n_flt);
        a_window.add(n_window);
        a_replayed.add(n_replayed);
        a_within.add(n_within);
        a_inwin.add(n_inwin);
        a_chain.add(n_chain);
    };
    const mpz_class n0_mpz = mpz_from_u128(node.n0);
    const mpz_class two_M_mpz = mpz_from_u128(two_M);

    if (node.f < fmin) {
        // Theorem 4.4: every extension is pruned (485 f <= 306 M). Certify
        // each with a GMP walk that must descend/merge by depth M. All of
        // them are checked: they are cheap and rare.
        for (unsigned a = 0; a < twoA; ++a) {
            ++n_excluded;
            ++n_flt;
            const mpz_class n = n0_mpz + mpz_class(a) * two_M_mpz;
            const Prefix p = build_prefix(n, M);
            if (!reduces_below(p, n, M)) {
                record_fail(collatz::to_string(node.n0),
                            collatz::oracle::to_decimal(n),
                            M,
                            "f<fmin: no descent within M steps");
            }
        }
        publish();
        return;
    }

    // test_unlifted_mask (test hook): the decisions of an unlifted survivor.
    const step2::CandidateMask mask =
        filter.mask(block && opts.test_unlifted_mask ? node_in : node);
    for (unsigned a = 0; a < twoA; ++a) {
        const u128 cand = node.n0 + (static_cast<u128>(a) << M);
        // test_drop_n (test hook): a must-check candidate dropped as if excluded.
        const bool dropped = opts.test_drop_n != 0 && cand == opts.test_drop_n;
        if (mask.test(a) && !dropped) {
            ++n_checked;
            continue;
        }
        ++n_excluded;

        if (mod9_cert(cand)) {
            ++n_mod9;
            continue; // mod-9 forward-image certificate (cheap, exact).
        }

        // Bitvector / partial-OEE window prune: the expensive GMP replay.
        ++n_window;
        // Sampling key: all of cand. In a full run hi64 == 0, so it is lo64(cand).
        const std::uint64_t skey =
            collatz::lo64(cand) ^ ((collatz::hi64(cand) << 32) | (collatz::hi64(cand) >> 32));
        if (opts.replay_sample > 1 && !dropped && (mix64(skey) % opts.replay_sample) != 0) {
            continue;
        }
        ++n_replayed;
        const mpz_class n = mpz_from_u128(cand);
        switch (replay_window(n, M, window_len, chain_depth)) {
        case Replay::within_m:
            ++n_within; // partial-OEE / descent at k<=M.
            break;
        case Replay::in_window:
            ++n_inwin; // bitvector window (M, M+B].
            break;
        case Replay::chain:
            // Window chain or merge-template term: a smaller ancestor y
            // (T^L(y)=T^s(n)) within the backward-preimage depth the dip can
            // use, 2*t_w for the window chains and up to D for the merge
            // templates.
            ++n_chain;
            break;
        case Replay::none:
            record_fail(collatz::to_string(node.n0),
                        collatz::oracle::to_decimal(n),
                        window_len,
                        "window: no descent/merge within N-A+B steps");
            break;
        }
    }
    publish();
}

} // namespace collatz::certify::detail
