/**
 * geometry.cpp — see motion/geometry.h.
 *
 * Transcribed from web/src/toolpath/geometry.ts. Expression ASSOCIATION is
 * load-bearing, not style: `3 * mt * mt * t * c.p1.x` must stay
 * `(((3*mt)*mt)*t)*p1.x` in both languages or the low bits diverge and the
 * golden byte-comparison stops meaning anything. Do not "tidy" the arithmetic.
 */

#include "motion/geometry.h"
#include "motion/jsmath.h"

#include <cmath>

namespace motion {

CubicBezier cubic(Pt p0, Pt p1, Pt p2, Pt p3) {
    return CubicBezier{p0, p1, p2, p3};
}

CubicBezier lineToCubic(Pt p0, Pt p1) {
    const double dx = (p1.x - p0.x) / 3;
    const double dy = (p1.y - p0.y) / 3;
    return CubicBezier{
        p0,
        Pt{p0.x + dx, p0.y + dy},
        Pt{p1.x - dx, p1.y - dy},
        p1,
    };
}

CubicBezier quadToCubic(Pt p0, Pt qp1, Pt p2) {
    // 2.0/3.0, NOT 2/3 — the latter is integer division in C++ and silently
    // collapses both control points onto the endpoints.
    return CubicBezier{
        p0,
        Pt{p0.x + (2.0 / 3.0) * (qp1.x - p0.x), p0.y + (2.0 / 3.0) * (qp1.y - p0.y)},
        Pt{p2.x + (2.0 / 3.0) * (qp1.x - p2.x), p2.y + (2.0 / 3.0) * (qp1.y - p2.y)},
        p2,
    };
}

// ── 2D vector algebra on Pt ──────────────────────────────────────────────────

Pt sub(Pt a, Pt b) { return Pt{a.x - b.x, a.y - b.y}; }

Pt add(Pt a, Pt b) { return Pt{a.x + b.x, a.y + b.y}; }

Pt scale(Pt v, double s) { return Pt{v.x * s, v.y * s}; }

double length(Pt v) {
    // sqrt(x*x + y*y), matching the TypeScript. NOT std::hypot — hypot is a
    // different (non-overflowing) algorithm and gives different low bits.
    return std::sqrt(v.x * v.x + v.y * v.y);
}

Pt normalize(Pt v) {
    const double l = length(v);
    if (l < 1e-12) return Pt{0, 0};
    return Pt{v.x / l, v.y / l};
}

double angleBetweenDeg(Pt u, Pt v) {
    const double dot = u.x * v.x + u.y * v.y;
    // jsMax/jsMin, not fmax/fmin: fmin(1, NaN) is 1 but Math.min(1, NaN) is
    // NaN. See motion/jsmath.h.
    const double clamped = jsMax(-1.0, jsMin(1.0, dot));
    return (jsAcos(clamped) * 180) / PI;
}

// ── Bezier endpoint tangents ─────────────────────────────────────────────────

Pt exitTangent(const CubicBezier& c) { return normalize(sub(c.p3, c.p2)); }

Pt entryTangent(const CubicBezier& c) { return normalize(sub(c.p1, c.p0)); }

// ── angle arithmetic ─────────────────────────────────────────────────────────

double angleDelta(double a, double b) {
    double d = b - a;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

// ── Bezier point evaluation + derivatives ────────────────────────────────────

Pt bezierPoint(const CubicBezier& c, double t) {
    const double mt = 1 - t;
    return Pt{
        mt * mt * mt * c.p0.x + 3 * mt * mt * t * c.p1.x + 3 * mt * t * t * c.p2.x + t * t * t * c.p3.x,
        mt * mt * mt * c.p0.y + 3 * mt * mt * t * c.p1.y + 3 * mt * t * t * c.p2.y + t * t * t * c.p3.y,
    };
}

Pt bezierDeriv1(const CubicBezier& c, double t) {
    const double mt = 1 - t;
    return Pt{
        3 * (mt * mt * (c.p1.x - c.p0.x) + 2 * mt * t * (c.p2.x - c.p1.x) + t * t * (c.p3.x - c.p2.x)),
        3 * (mt * mt * (c.p1.y - c.p0.y) + 2 * mt * t * (c.p2.y - c.p1.y) + t * t * (c.p3.y - c.p2.y)),
    };
}

Pt bezierDeriv2(const CubicBezier& c, double t) {
    const double mt = 1 - t;
    return Pt{
        6 * (mt * (c.p2.x - 2 * c.p1.x + c.p0.x) + t * (c.p3.x - 2 * c.p2.x + c.p1.x)),
        6 * (mt * (c.p2.y - 2 * c.p1.y + c.p0.y) + t * (c.p3.y - 2 * c.p2.y + c.p1.y)),
    };
}

double curvature(const CubicBezier& c, double t) {
    const Pt d1 = bezierDeriv1(c, t);
    const Pt d2 = bezierDeriv2(c, t);
    const double cross = d1.x * d2.y - d1.y * d2.x;
    const double speed = std::sqrt(d1.x * d1.x + d1.y * d1.y);
    if (speed < 1e-10) return 0;
    return std::fabs(cross) / (speed * speed * speed);
}

} // namespace motion
