#ifndef COLLATZ_PREIMAGE_CHAIN_HPP
#define COLLATZ_PREIMAGE_CHAIN_HPP

// Generalized preimage-chain merging mod 3^t.
//
// Builds a table, keyed by a value's residue mod 3^t, of the best
// (smallest-ratio) backward Collatz chain that lands on a strictly smaller
// integer. The table drives a Step-1 sieve that generalizes Path-Merging (the
// t = 1, pure-odd, one-step case).
//
// Backward steps. The accelerated map is T(n) = n/2 (n even) or (3n+1)/2
// (n odd). A value w has two kinds of T-preimage (values u with T(u) = w):
//
//   * even preimage: u = 2w.  Always an integer; ratio u/w = 2 (value grows).
//   * odd  preimage: u = (2w-1)/3.  An integer iff w == 2 (mod 3); it is then
//                    automatically odd (odd/odd), and T(u) = w. Ratio ~2/3
//                    (value shrinks). This is the only branch that can merge n
//                    with a smaller number.
//
// A "chain" is a sequence of such backward steps from a start value x. After a
// chain with L total steps and v odd steps, the reached value is the exact
// affine function of x
//
//     A(x) = (2^L * x - K) / 3^v ,     with T^L(A(x)) = x,
//
// where K >= 0 is a constant fixed by the sequence of even/odd steps (not by
// x). Whether the sequence stays integral (each odd step's w == 2 (mod 3)
// holds) and the value of K depend only on x mod 3^v; since v <= t, the table
// is keyed by x mod 3^t. The chain's ratio is r = 2^L / 3^v, and it lands
// below x iff r < 1 (i.e. 2^L < 3^v). Pure-odd chains (L = v) give the steepest
// ratio (2/3)^v; mixed chains (an even step enabling further odd steps) give
// shallower ratios such as 8/9 = 2^3/3^2 that reach residues pure-odd cannot
// (e.g. m == 1 (mod 3), which Path-Merging never touches).
//
// Sound Step-1 exclusion rule (see ChainEntry). In the Step-1 DFS a node
// is (n0, m, k, f) with m = T^k(n0), f = #odd steps in those k steps,
// representing the class n = n0 + a*2^k (a >= 0). By the affine lemma
// T^k(n) = m + 3^f * a. Excluding the whole class with a chain chosen from m's
// residue requires, for every a:
//
//   (i)   residue uniformity: T^k(n) == m (mod 3^v) for all a. Since the term
//         is 3^f*a, this holds for all a iff f >= v. (So the DFS gates on f,
//         not only on n0.)
//   (ii)  base: A(m) = (2^L m - K)/3^v < n0.
//   (iii) slope: the ancestor A(m + 3^f a) = A(m) + 2^L*3^{f-v}*a must stay
//         below n = n0 + 2^k a for all a, which given (ii) holds iff
//         2^L * 3^{f-v} <= 2^k.
//
// When (i)-(iii) hold, every class member n has the integer ancestor
// A(T^k(n)) < n whose forward orbit passes through T^k(n) = T^k(n0)+3^f a, hence
// merges with n; by the outer induction (all naturals < n known to converge)
// n converges. (For t = 1 this is Path-Merging: L=v=1, K=1, A(m)=(2m-1)/3, and
// (iii) is implied by (ii), as in the path-merge sieve.)
//
// collatz/preimage_chain/chain_table.hpp declares the Step-1 chain table and
// collatz/preimage_chain/window_chain_table.hpp the window gain table of the
// Step-2 dip.

#include "collatz/preimage_chain/chain_table.hpp"
#include "collatz/preimage_chain/window_chain_table.hpp"

#endif // COLLATZ_PREIMAGE_CHAIN_HPP
