/**
 * line.h — a straight XY move and its speed limits.
 *
 * Per-axis limits are projected onto the line's direction: an axis carrying a
 * fraction |u_i| of the motion allows the line max_i / |u_i|.
 */

#ifndef PLANNER_LINE_H
#define PLANNER_LINE_H

#include "planner/path.h"

namespace planner {

struct Line {
    Vec2 p0, p1;         // mm, machine frame
    Vec2 dir;            // unit vector p0 → p1
    float length = 0;    // mm
    float v_max_sqr = 0; // requested feed, capped by the axes
    float accel = 0;     // mm/s² along the line
};

/** `length` is 0 when p0 == p1; `dir`, `v_max_sqr` and `accel` are then meaningless. */
Line makeLine(Vec2 p0, Vec2 p1, float feed, const AxisLimits& limits);

Path pathOf(const Line& ln);

/** A Z or A move of `d` (mm or deg, signed), capped by that axis. */
Path axisPath(Axes axes, float d, float feed, const AxisLimits& limits);

/** Highest squared speed through the corner from `prev` into `next`. */
float junctionMaxSqr(const Line& prev, const Line& next, float deviation);

}  // namespace planner

#endif
