/**
 * path.h — what look-ahead and junctions need from any block, line or Bézier.
 *
 * Corners use junction deviation (GRBL) on the end tangents: the speed at which
 * a circle of radius set by `deviation` could round the corner at the lower of
 * the two accelerations. A tangent-continuous join costs nothing; only the
 * neighbours' own caps limit it.
 */

#ifndef PLANNER_PATH_H
#define PLANNER_PATH_H

namespace planner {

struct Vec2 {
    float x = 0;
    float y = 0;
};

struct AxisLimits {
    float max_feed[2] = {0, 0};    // mm/s, X and Y
    float max_accel[2] = {0, 0};   // mm/s²
};

struct Path {
    float length = 0;     // mm
    float accel = 0;      // mm/s² along the path
    float v_max_sqr = 0;  // requested feed, capped by the axes and curvature
    Vec2 dir_start;       // unit tangents
    Vec2 dir_end;
    Vec2 end;             // mm, machine frame
};

/** Highest squared speed through the corner from `prev` into `next`. */
float junctionMaxSqr(const Path& prev, const Path& next, float deviation);

}  // namespace planner

#endif
