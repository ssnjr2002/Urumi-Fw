/**
 * jsmath.h — JavaScript Math semantics that C++ does not share.
 *
 * The port's contract is bit-equality with the TypeScript, so where `Math.x`
 * and the obvious `std::x` disagree, the TypeScript wins. Every shim here
 * exists because a specific `std::` function is a FALSE FRIEND: it compiles,
 * it reads correctly, and it is wrong on some part of the input domain.
 *
 * Two kinds live here. The inline ones below are semantic mismatches — tie
 * direction, NaN handling, signed zero. The three declared at the bottom are
 * NUMERIC mismatches: the platform libm computes a different 1-ULP answer than
 * V8 does, on 17.6% of atan2 inputs and 7.7% of acos inputs. Those are
 * implemented in jsmath.cpp, which explains why the port has to own them and
 * why it matters beyond testing.
 *
 * See docs/planner_audit.md, "Numeric porting rule".
 */

#ifndef MOTION_JSMATH_H
#define MOTION_JSMATH_H

#include <cmath>
#include <limits>

namespace motion {

/**
 * Math.round. Three ways to get this wrong, in decreasing order of how obvious
 * they are:
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
inline double jsRound(double x) {
    const double f = std::floor(x);
    const double r = (x - f >= 0.5) ? f + 1.0 : f;
    return r == 0.0 ? std::copysign(0.0, x) : r;
}

/**
 * Math.min / Math.max.
 *
 * `std::fmin`/`std::fmax` are the false friends: they deliberately IGNORE a
 * NaN operand and return the other one, where JS propagates it.
 * `std::min`/`std::max` are wrong differently — they return the first argument
 * on any unordered comparison. Both also disagree with JS on signed zero:
 * Math.min(0, -0) is -0 and Math.max(-0, 0) is +0.
 *
 * NaN should never reach these in the port's scope, which is exactly why the
 * shim is cheap insurance. If one ever does, it surfaces as a divergence from
 * the TypeScript instead of as a silently different toolpath.
 */
inline double jsMin(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? a : b; // -0 wins
    return a < b ? a : b;
}

inline double jsMax(double a, double b) {
    if (std::isnan(a) || std::isnan(b)) return std::numeric_limits<double>::quiet_NaN();
    if (a == 0.0 && b == 0.0) return std::signbit(a) ? b : a; // +0 wins
    return a > b ? a : b;
}

// ── owned transcendentals (jsmath.cpp) ───────────────────────────────────────
// Not shims around std:: — full implementations, because no platform libm
// agrees with V8 and the Pico's newlib would be a third answer again. Each is
// verified bit-identical to V8 over 200,000 inputs.

/** Math.atan2. fdlibm, which is what V8's ieee754.cc derives from. */
double jsAtan2(double y, double x);

/** Math.acos. fdlibm likewise. */
double jsAcos(double x);

/** Math.hypot. V8's own algorithm (scale by max + Kahan), NOT std::hypot. */
double jsHypot(double a, double b);

} // namespace motion

#endif // MOTION_JSMATH_H
