#include "collatz/frontier.hpp"

#include "collatz/pow3.hpp"

namespace collatz::frontier {

Lift make_lift(u128 h, unsigned K, unsigned A) noexcept {
    Lift l;
    l.L = h << K;
    l.h2A = h << A;
    return l;
}

void lift_survivor(step1::Node& node, const Lift& lift) noexcept {
    node.n0 += lift.L;
    node.m += pow3(node.f) * lift.h2A;
    node.pmax += node.smax * lift.h2A;
    if (node.oee_astar != step1::kOeeAStarNone) {
        node.oee_astar = node.oee_astar > lift.h2A ? node.oee_astar - lift.h2A : 0;
    }
}

} // namespace collatz::frontier
