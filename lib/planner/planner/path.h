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

/** A machine position, one coordinate per axis. Not a vector: A is in degrees. */
struct Pos {
    float x = 0, y = 0, z = 0;   // mm
    float a = 0;                 // deg
    Vec2 xy() const { return {x, y}; }
    void setXy(Vec2 q) { x = q.x; y = q.y; }
};

struct AxisLimits {
    float max_feed[4] = {0, 0, 0, 0};    // mm/s (A: deg/s), X Y Z A
    float max_accel[4] = {0, 0, 0, 0};   // mm/s² (A: deg/s²)
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
