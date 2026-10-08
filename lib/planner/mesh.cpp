#include "planner/mesh.h"
#include "planner/ram.h"

#include <math.h>

namespace planner {

bool Mesh::set(int nx_, int ny_, float x0_, float y0_, float dx_, float dy_, const int16_t* heights) {
    z = nullptr;
    if (nx_ < 2 || ny_ < 2 || !(dx_ > 0) || !(dy_ > 0) || !heights) return false;
    nx = nx_;
    ny = ny_;
    x0 = x0_;
    y0 = y0_;
    dx = dx_;
    dy = dy_;
    inv_dx = 1 / dx_;
    inv_dy = 1 / dy_;
    z = heights;
    return true;
}

// Cell index and fraction along one axis, clamped to the grid. No libm: the
// clamp makes the cast a floor.
PLANNER_RAM static inline int cell(float u, int n, float& f) {
    if (!(u > 0)) { f = 0; return 0; }
    const float last = (float)(n - 1);
    if (u >= last) { f = 1; return n - 2; }
    int i = (int)u;
    if (i > n - 2) i = n - 2;
    f = u - (float)i;
    return i;
}

PLANNER_RAM float meshAt(const Mesh& m, float x, float y) {
    float fx, fy;
    const int i = cell((x - m.x0) * m.inv_dx, m.nx, fx);
    const int j = cell((y - m.y0) * m.inv_dy, m.ny, fy);
    const int16_t* r0 = m.z + j * m.nx + i;
    const int16_t* r1 = r0 + m.nx;
    const float a = (float)r0[0] + fx * (float)(r0[1] - r0[0]);
    const float b = (float)r1[0] + fx * (float)(r1[1] - r1[0]);
    return (a + fy * (b - a)) * 0.001f;
}

namespace {

template <typename At>
MeshWalk walk(const Mesh& m, float length, Vec2 tip, float ref, At at) {
    MeshWalk w;
    const Vec2 p0 = at(0);
    w.lo = w.hi = meshAt(m, p0.x + tip.x, p0.y + tip.y) - ref;
    if (!(length > 0)) return w;
    const float h = 0.5f * fminf(m.dx, m.dy);
    const int n = (int)ceilf(length / h);
    w.step = length / (float)n;
    float z_prev = w.lo, k_prev = 0;
    for (int i = 1; i <= n; i++) {
        const Vec2 p = at(w.step * (float)i);
        const float z = meshAt(m, p.x + tip.x, p.y + tip.y) - ref;
        const float k = (z - z_prev) / w.step;
        w.lo = fminf(w.lo, z);
        w.hi = fmaxf(w.hi, z);
        w.slope = fmaxf(w.slope, fabsf(k));
        if (i > 1) w.dslope = fmaxf(w.dslope, fabsf(k - k_prev));
        z_prev = z;
        k_prev = k;
    }
    return w;
}

}  // namespace

MeshWalk meshWalkLine(const Mesh& m, Vec2 p0, Vec2 p1, Vec2 tip, float ref) {
    const float ex = p1.x - p0.x, ey = p1.y - p0.y;
    const float len = sqrtf(ex * ex + ey * ey);
    const float ux = len > 0 ? ex / len : 0, uy = len > 0 ? ey / len : 0;
    return walk(m, len, tip, ref, [&](float s) { return Vec2{p0.x + ux * s, p0.y + uy * s}; });
}

MeshWalk meshWalkBezier(const Mesh& m, const Bezier& b, Vec2 tip, float ref) {
    return walk(m, b.length, tip, ref, [&](float s) { return bezierPoint(b, bezierT(b, s)); });
}

PathCap meshCap(const MeshWalk& w, float z_feed, float z_accel) {
    PathCap c;
    if (w.slope > 0) {
        const float v = z_feed / w.slope;
        c.v_max_sqr = v * v;
        c.accel = 0.5f * z_accel / w.slope;
    }
    if (w.dslope > 0) c.v_max_sqr = fminf(c.v_max_sqr, 0.5f * z_accel * w.step / w.dslope);
    return c;
}

}  // namespace planner
