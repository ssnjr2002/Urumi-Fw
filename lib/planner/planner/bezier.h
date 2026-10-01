/**
 * bezier.h — a cubic Bézier XY move: its analysis, evaluation and speed limits.
 *
 * Position along the curve is `B(t(s))`, with `t(s) = c1·s + c2·s² + c3·s³`
 * fitted to the arc length (seed §2). A fit error only shifts position along
 * the exact curve (speed ripple), never off it. c1 is chosen so t(length) = 1.
 *
 * Limits use the direction-independent inscribed circle of the per-axis box:
 * a = min(max_accel), v ≤ min(max_feed). Tangential and centripetal
 * acceleration each get a/√2, so their vector sum never exceeds a, a hold
 * included: accel = a/√2 and v_max² ≤ (a/√2) / κ_max (seed §7, XY only).
 */

#ifndef PLANNER_BEZIER_H
#define PLANNER_BEZIER_H

#include "planner/path.h"

namespace planner {

struct Bezier {
    Vec2 p[4];               // mm, machine frame
    float ts[3] = {0, 0, 0}; // t(s) coefficients c1, c2, c3
    float length = 0;        // mm, arc length
    float kappa_max = 0;     // 1/mm, max |κ|
    float dkappa_max = 0;    // 1/mm², max |dκ/ds|
    float kappa_start = 0;   // signed, + turns left
    float kappa_end = 0;
};

enum class BezierError {
    None,
    DegenerateHandle,   // p1 on p0 or p2 on p3: no end tangent
    Cusp,               // the curve stops somewhere inside
    NonMonotonic,       // the t(s) fit runs backwards
    Inconsistent,       // host numbers that cannot belong to these control points
};

/**
 * Fill `out` from the control points. `fit_error`, if given, receives the
 * largest |t_fit(s) − t(s)| over the samples.
 */
BezierError analyzeBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, Bezier& out,
                          float* fit_error = nullptr);

/**
 * Accept a curve the host analysed: `b` holds p, length, kappa_max,
 * dkappa_max, ts[1] and ts[2]. Derives ts[0] and the end curvatures, and
 * checks the rest against the control points cheaply instead of analysing.
 */
BezierError checkBezier(Bezier& b);

float bezierT(const Bezier& b, float s);        // clamped to [0, 1]
Vec2 bezierPoint(const Bezier& b, float t);
Vec2 bezierDeriv(const Bezier& b, float t);     // dB/dt

Path pathOf(const Bezier& b, float feed, const AxisLimits& limits);

}  // namespace planner

#endif
