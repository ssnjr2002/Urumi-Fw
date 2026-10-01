#include "planner/bezier.h"
#include "planner/ram.h"

#include <math.h>

namespace planner {

namespace {

// Samples in t for the analysis; each interval integrated by 3-point Gauss-Legendre.
constexpr int kN = 128;

Vec2 sub(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }
float norm(Vec2 v) { return sqrtf(v.x * v.x + v.y * v.y); }

Vec2 deriv2(const Vec2* p, float t) {
    const float u = 1 - t;
    return {6 * (u * (p[2].x - 2 * p[1].x + p[0].x) + t * (p[3].x - 2 * p[2].x + p[1].x)),
            6 * (u * (p[2].y - 2 * p[1].y + p[0].y) + t * (p[3].y - 2 * p[2].y + p[1].y))};
}

float curvature(Vec2 d, Vec2 dd) {
    const float n = norm(d);
    return (d.x * dd.y - d.y * dd.x) / (n * n * n);
}

float curvature(const Bezier& b, float t) { return curvature(bezierDeriv(b, t), deriv2(b.p, t)); }

// How far κ = d × dd / |d|³ can move when every control point moves by up to
// delta: d moves by up to 6·delta, dd by up to 24·delta.
float curvatureSlack(Vec2 d, Vec2 dd, float k, float delta) {
    const float n = norm(d);
    return delta * (24 * n + 6 * norm(dd)) / (n * n * n) + 18 * fabsf(k) * delta / n;
}

constexpr float kHandleEps = 1e-4f;   // mm

// t(u) = u + d2·(u² − u) + d3·(u³ − u): dt/du = 1 + d2·(2u − 1) + d3·(3u² − 1)
// must stay positive on [0, 1].
bool monotonic(float d2, float d3) {
    float slope_min = fminf(1 - d2 - d3, 1 + d2 + 2 * d3);
    if (d3 != 0) {
        const float uv = -d2 / (3 * d3);
        if (uv > 0 && uv < 1) slope_min = fminf(slope_min, 1 + d2 * (2 * uv - 1) + d3 * (3 * uv * uv - 1));
    }
    return slope_min > 0;
}

}  // namespace

PLANNER_RAM Vec2 bezierPoint(const Bezier& b, float t) {
    if (t <= 0) return b.p[0];
    if (t >= 1) return b.p[3];
    const float u = 1 - t;
    const float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
    return {w0 * b.p[0].x + w1 * b.p[1].x + w2 * b.p[2].x + w3 * b.p[3].x,
            w0 * b.p[0].y + w1 * b.p[1].y + w2 * b.p[2].y + w3 * b.p[3].y};
}

Vec2 bezierDeriv(const Bezier& b, float t) {
    const float u = 1 - t;
    const float w0 = 3 * u * u, w1 = 6 * u * t, w2 = 3 * t * t;
    return {w0 * (b.p[1].x - b.p[0].x) + w1 * (b.p[2].x - b.p[1].x) + w2 * (b.p[3].x - b.p[2].x),
            w0 * (b.p[1].y - b.p[0].y) + w1 * (b.p[2].y - b.p[1].y) + w2 * (b.p[3].y - b.p[2].y)};
}

PLANNER_RAM float bezierT(const Bezier& b, float s) {
    const float t = s * (b.ts[0] + s * (b.ts[1] + s * b.ts[2]));
    return t < 0 ? 0 : (t > 1 ? 1 : t);
}

BezierError analyzeBezier(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, Bezier& out,
                          float* fit_error) {
    Bezier b;
    b.p[0] = p0;
    b.p[1] = p1;
    b.p[2] = p2;
    b.p[3] = p3;
    if (norm(sub(p1, p0)) < kHandleEps || norm(sub(p3, p2)) < kHandleEps) return BezierError::DegenerateHandle;

    // Cumulative arc length at t_i = i/kN.
    static const float gx[3] = {-0.7745966692f, 0, 0.7745966692f};
    static const float gw[3] = {5.0f / 9, 8.0f / 9, 5.0f / 9};
    float s[kN + 1];
    float min_speed = INFINITY;
    s[0] = 0;
    for (int i = 0; i < kN; i++) {
        const float t0 = float(i) / kN, h = 1.0f / kN;
        float acc = 0;
        for (int k = 0; k < 3; k++) acc += gw[k] * norm(bezierDeriv(b, t0 + h * 0.5f * (gx[k] + 1)));
        s[i + 1] = s[i] + acc * h * 0.5f;
        min_speed = fminf(min_speed, norm(bezierDeriv(b, t0)));
    }
    min_speed = fminf(min_speed, norm(bezierDeriv(b, 1)));
    b.length = s[kN];
    // |B'| averages `length` over t; a small fraction of that is a stop.
    if (min_speed < 1e-3f * b.length) return BezierError::Cusp;

    // Curvature extremes and its rate along s.
    float k_prev = curvature(b, 0);
    b.kappa_start = k_prev;
    b.kappa_max = fabsf(k_prev);
    for (int i = 1; i <= kN; i++) {
        const float k = curvature(b, float(i) / kN);
        b.kappa_max = fmaxf(b.kappa_max, fabsf(k));
        b.dkappa_max = fmaxf(b.dkappa_max, fabsf(k - k_prev) / (s[i] - s[i - 1]));
        k_prev = k;
    }
    b.kappa_end = k_prev;

    // Fit t(u) = u + d2·(u² − u) + d3·(u³ − u), u = s/length: exact at both
    // ends, least squares on the rest.
    float a22 = 0, a23 = 0, a33 = 0, r2 = 0, r3 = 0;
    for (int i = 1; i < kN; i++) {
        const float u = s[i] / b.length;
        const float f2 = u * u - u, f3 = u * u * u - u;
        const float r = float(i) / kN - u;
        a22 += f2 * f2;
        a23 += f2 * f3;
        a33 += f3 * f3;
        r2 += f2 * r;
        r3 += f3 * r;
    }
    const float det = a22 * a33 - a23 * a23;
    const float d2 = det != 0 ? (r2 * a33 - r3 * a23) / det : 0;
    const float d3 = det != 0 ? (a22 * r3 - a23 * r2) / det : 0;

    if (!monotonic(d2, d3)) return BezierError::NonMonotonic;

    const float L = b.length;
    b.ts[0] = (1 - d2 - d3) / L;
    b.ts[1] = d2 / (L * L);
    b.ts[2] = d3 / (L * L * L);

    if (fit_error) {
        float e = 0;
        for (int i = 0; i <= kN; i++) e = fmaxf(e, fabsf(bezierT(b, s[i]) - float(i) / kN));
        *fit_error = e;
    }
    out = b;
    return BezierError::None;
}

BezierError checkBezier(Bezier& b) {
    const Vec2* p = b.p;
    if (norm(sub(p[1], p[0])) < kHandleEps || norm(sub(p[3], p[2])) < kHandleEps)
        return BezierError::DegenerateHandle;

    // Slack for host doubles rounded to the wire's floats: relative for the
    // scalars, and delta (four float steps of the largest coordinate) for
    // anything derived from the points.
    constexpr float kRel = 1e-4f;
    float extent = 1;
    for (int i = 0; i < 4; i++) extent = fmaxf(extent, fmaxf(fabsf(p[i].x), fabsf(p[i].y)));
    const float delta = extent * 2.4e-7f;

    const float L = b.length;
    const float chord = norm(sub(p[3], p[0]));
    const float polygon = norm(sub(p[1], p[0])) + norm(sub(p[2], p[1])) + norm(sub(p[3], p[2]));
    // Written so a NaN anywhere fails.
    if (!(L >= chord * (1 - kRel) - 3 * delta && L <= polygon * (1 + kRel) + 9 * delta))
        return BezierError::Inconsistent;
    if (!(b.dkappa_max >= 0)) return BezierError::Inconsistent;

    const float d2 = b.ts[1] * L * L, d3 = b.ts[2] * L * L * L;
    if (!monotonic(d2, d3)) return BezierError::NonMonotonic;
    b.ts[0] = (1 - d2 - d3) / L;

    // A kappa_max short of an end by no more than the slack is raised to it,
    // so the planner never runs below the curvature it derives.
    const Vec2 ends[2][2] = {{bezierDeriv(b, 0), deriv2(p, 0)}, {bezierDeriv(b, 1), deriv2(p, 1)}};
    float* const k_ends[2] = {&b.kappa_start, &b.kappa_end};
    for (int i = 0; i < 2; i++) {
        const float k = curvature(ends[i][0], ends[i][1]);
        const float k_abs = fabsf(k);
        *k_ends[i] = k;
        if (!(b.kappa_max >= k_abs * (1 - kRel) - curvatureSlack(ends[i][0], ends[i][1], k, delta)))
            return BezierError::Inconsistent;
        b.kappa_max = fmaxf(b.kappa_max, k_abs);
    }
    return BezierError::None;
}

Path pathOf(const Bezier& b, float feed, const AxisLimits& limits) {
    Path p;
    p.length = b.length;
    p.end = b.p[3];
    const Vec2 d0 = sub(b.p[1], b.p[0]), d1 = sub(b.p[3], b.p[2]);
    const float n0 = norm(d0), n1 = norm(d1);
    p.dir_start = {d0.x / n0, d0.y / n0};
    p.dir_end = {d1.x / n1, d1.y / n1};

    const float a = fminf(limits.max_accel[0], limits.max_accel[1]) * 0.70710678f;
    const float v = fminf(feed, fminf(limits.max_feed[0], limits.max_feed[1]));
    p.accel = a;
    p.v_max_sqr = v * v;
    if (b.kappa_max > 0) p.v_max_sqr = fminf(p.v_max_sqr, a / b.kappa_max);
    return p;
}

}  // namespace planner
