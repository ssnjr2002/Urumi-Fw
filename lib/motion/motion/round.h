/**
 * round.h — JavaScript's Math.round, which is not C++'s std::round.
 *
 * docs/planner_audit.md, "Math.round is not std::round". Three ways to get this
 * wrong, in decreasing order of how obvious they are:
 *
 *   1. `std::round` ties AWAY FROM ZERO; `Math.round` ties toward +infinity.
 *      std::round(-1.5) == -2, Math.round(-1.5) == -1. There are 24 rounding
 *      sites in the port's scope and several take signed values.
 *
 *   2. `std::floor(x + 0.5)` fixes the tie direction but is still not
 *      Math.round: for x = 0.49999999999999994 (the double just below 1/2),
 *      x + 0.5 rounds UP to exactly 1.0 before floor sees it, giving 1 where
 *      JS gives 0. Compare the fractional part instead of perturbing x.
 *
 *   3. Math.round(-0.2) and Math.round(-0.5) are both NEGATIVE zero. The
 *      differential test caught this on its first run: every caller in the
 *      port's scope feeds the result to an integer step count, where the two
 *      zeros are indistinguishable, so it would have been fair to exempt. It
 *      is not exempted, because the value of a bit-equality harness is that it
 *      has no exemptions to argue about, and copysign is one instruction on a
 *      branch that is already returning zero.
 */

#ifndef MOTION_ROUND_H
#define MOTION_ROUND_H

#include <cmath>

namespace motion {

/** Bit-exact JavaScript Math.round, for transcription fidelity. */
inline double jsRound(double x) {
    const double f = std::floor(x);
    const double r = (x - f >= 0.5) ? f + 1.0 : f;
    return r == 0.0 ? std::copysign(0.0, x) : r;
}

} // namespace motion

#endif // MOTION_ROUND_H
