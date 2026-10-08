#include "planner/line.h"

#include <math.h>

namespace planner {

Line makeLine(Vec2 p0, Vec2 p1, float feed, const AxisLimits& limits) {
    Line ln;
    ln.p0 = p0;
    ln.p1 = p1;
    const float dx = p1.x - p0.x;
    const float dy = p1.y - p0.y;
    ln.length = sqrtf(dx * dx + dy * dy);
    if (ln.length <= 0) return ln;

    ln.dir = {dx / ln.length, dy / ln.length};
    const float u[2] = {fabsf(ln.dir.x), fabsf(ln.dir.y)};

    float v = feed;
    float a = INFINITY;
    for (int i = 0; i < 2; i++) {
        if (u[i] <= 0) continue;
        v = fminf(v, limits.max_feed[i] / u[i]);
        a = fminf(a, limits.max_accel[i] / u[i]);
    }
    ln.v_max_sqr = v * v;
    ln.accel = a;
    return ln;
}

Axes axesOf(const Pos& from, const Pos& to) {
    const bool xy = to.x != from.x || to.y != from.y;
    const bool z = to.z != from.z;
    const bool a = to.aSince(from) != 0;
    if (xy + z + a > 1) return AXES_MIXED;
    return xy ? AXES_XY : z ? AXES_Z : a ? AXES_A : AXES_NONE;
}

Path axisPath(Axes axes, float d, float feed, const AxisLimits& limits) {
    const int i = axes == AXES_Z ? 2 : 3;
    const float v = fminf(feed, limits.max_feed[i]);
    Path p;
    p.axes = axes;
    p.length = fabsf(d);
    p.accel = limits.max_accel[i];
    p.v_max_sqr = v * v;
    p.dir_start = p.dir_end = {d < 0 ? -1.0f : 1.0f, 0};
    return p;
}

Path pathOf(const Line& ln) {
    Path p;
    p.length = ln.length;
    p.accel = ln.accel;
    p.v_max_sqr = ln.v_max_sqr;
    p.dir_start = ln.dir;
    p.dir_end = ln.dir;
    p.end = ln.p1;
    return p;
}

float junctionMaxSqr(const Path& prev, const Path& next, float deviation) {
    if (prev.axes != next.axes) return 0;
    const float cap = fminf(prev.v_max_sqr, next.v_max_sqr);
    // Cosine of the angle between the incoming reversed and the outgoing
    // direction: -1 is straight on, +1 is a full reversal.
    const float cos_theta = -(prev.dir_end.x * next.dir_start.x + prev.dir_end.y * next.dir_start.y);
    if (cos_theta < -0.999999f) return cap;
    if (cos_theta > 0.999999f) return 0;

    const float sin_half = sqrtf(0.5f * (1.0f - cos_theta));
    const float a = fminf(prev.accel, next.accel);
    const float v_sqr = a * deviation * sin_half / (1.0f - sin_half);
    return fminf(cap, v_sqr);
}

float junctionMaxSqr(const Line& prev, const Line& next, float deviation) {
    return junctionMaxSqr(pathOf(prev), pathOf(next), deviation);
}

}  // namespace planner
