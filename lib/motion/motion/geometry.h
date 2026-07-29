/**
 * geometry.h — cubic Bezier primitives, 2D vector algebra, Bezier math.
 *
 * Transcribed from web/src/toolpath/geometry.ts. The TypeScript remains the
 * reference implementation until the port is complete; this file must stay a
 * BIT-EXACT transcription of it, not an improvement on it. See
 * docs/planner_audit.md, "Numeric porting rule":
 *
 *   - `double` throughout, never `float`. The RP2350 planner budget is
 *     ~230k cycles per microsegment and one sample costs ~25k in double.
 *     float32 would buy ~9% of an idle core and cost byte-comparability
 *     against the TypeScript golden, which is the port's only mechanical
 *     attribution signal.
 *   - Transcendentals are the DOUBLE libm ones (`std::hypot`, not `hypotf`).
 *     Measured bit-identical to V8's across the port's whole surface.
 *   - `std::round` is NOT `Math.round` (ties away from zero vs toward +inf).
 *     Not used here; see motion/round.h for the faithful idiom.
 *
 * Deliberately absent, as in the TypeScript: there is no arcLength(). A
 * 5-point Gauss-Legendre quadrature lived there with no production caller
 * (audit F6) and was removed rather than ported.
 */

#ifndef MOTION_GEOMETRY_H
#define MOTION_GEOMETRY_H

namespace motion {

struct Pt {
    double x;
    double y;
};

struct CubicBezier {
    Pt p0;
    Pt p1;
    Pt p2;
    Pt p3;
};

/** Cubic Bezier approximation constant for a quarter-circle arc. */
constexpr double KAPPA = 0.5522847498;

CubicBezier cubic(Pt p0, Pt p1, Pt p2, Pt p3);

/** Degenerate cubic from a line: control points on the line at 1/3 and 2/3. */
CubicBezier lineToCubic(Pt p0, Pt p1);

/** Degree elevation: quadratic -> cubic. */
CubicBezier quadToCubic(Pt p0, Pt qp1, Pt p2);

// ── 2D vector algebra on Pt ──────────────────────────────────────────────────

Pt sub(Pt a, Pt b);
Pt add(Pt a, Pt b);
Pt scale(Pt v, double s);
double length(Pt v);

/** Unit vector; returns {0, 0} for near-zero input. */
Pt normalize(Pt v);

/** Signed angle from u to v in degrees, range [0, 180]. */
double angleBetweenDeg(Pt u, Pt v);

// ── Bezier endpoint tangents ─────────────────────────────────────────────────

/** Unit tangent leaving curve c (direction p2 -> p3). Normalized B'(1). */
Pt exitTangent(const CubicBezier& c);

/** Unit tangent entering curve c (direction p0 -> p1). Normalized B'(0). */
Pt entryTangent(const CubicBezier& c);

// ── angle arithmetic ─────────────────────────────────────────────────────────

/** Shortest signed rotation from angle a to angle b in degrees, range +-180. */
double angleDelta(double a, double b);

// ── Bezier point evaluation + derivatives ────────────────────────────────────

/** B(t) — De Casteljau evaluation of the cubic at parameter t in [0,1]. */
Pt bezierPoint(const CubicBezier& c, double t);

/** B'(t) — first derivative. */
Pt bezierDeriv1(const CubicBezier& c, double t);

/** B''(t) — second derivative. */
Pt bezierDeriv2(const CubicBezier& c, double t);

/** kappa(t) = |B'xB''| / |B'|^3. Returns 0 for near-zero speed. */
double curvature(const CubicBezier& c, double t);

} // namespace motion

#endif // MOTION_GEOMETRY_H
