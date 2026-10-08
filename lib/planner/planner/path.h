/**
 * path.h — what look-ahead and junctions need from any block, line or Bézier.
 *
 * Corners use junction deviation (GRBL) on the end tangents: the speed at which
 * a circle of radius set by `deviation` could round the corner at the lower of
 * the two accelerations. A tangent-continuous join costs nothing; only the
 * neighbours' own caps limit it.
 *
 * A path moves one axis set: XY, Z alone or A alone. Joins between different
 * sets stop. A Z or A path's direction is {±1, 0}, so the same formula runs
 * straight on at full speed and stops on a reversal.
 */

#ifndef PLANNER_PATH_H
#define PLANNER_PATH_H

#include <math.h>
#include <stdint.h>

namespace planner {

struct Vec2 {
    float x = 0;
    float y = 0;
};

/**
 * A machine position, one coordinate per axis. Not a vector: A is in degrees.
 * A is a heading in [0, 360) plus whole turns, so its resolution does not fall
 * as the turns add up.
 */
struct Pos {
    float x = 0, y = 0, z = 0;   // mm
    float a = 0;                 // deg, [0, 360)
    int32_t turns = 0;
    Vec2 xy() const { return {x, y}; }
    void setXy(Vec2 q) { x = q.x; y = q.y; }
    /** Degrees from `o` to here. */
    float aSince(const Pos& o) const { return (float)(turns - o.turns) * 360.0f + (a - o.a); }
    /** Turn A by `deg`, either way. */
    void addA(float deg) {
        const float t = a + deg;
        const float k = floorf(t / 360.0f);
        a = t - k * 360.0f;
        turns += (int32_t)k;
        if (a >= 360.0f) { a -= 360.0f; turns++; }
        if (a < 0.0f) { a += 360.0f; turns--; }
    }
};

/** The axis set a path moves. */
enum Axes : uint8_t { AXES_XY, AXES_Z, AXES_A, AXES_NONE, AXES_MIXED };

/** Which set the move from `from` to `to` uses: NONE if it stays put. */
Axes axesOf(const Pos& from, const Pos& to);

struct AxisLimits {
    float max_feed[4] = {0, 0, 0, 0};    // mm/s (A: deg/s), X Y Z A
    float max_accel[4] = {0, 0, 0, 0};   // mm/s² (A: deg/s²)
};

struct Path {
    Axes axes = AXES_XY;
    float length = 0;     // mm (A: deg)
    float accel = 0;      // along the path, per s²
    float v_max_sqr = 0;  // requested feed, capped by the axes and curvature
    Vec2 dir_start;       // unit tangents; Z and A: {±1, 0}
    Vec2 dir_end;
    Vec2 end;             // XY: mm, machine frame
};

/** Caps a caller adds to a path beyond its axes: another axis riding along. */
struct PathCap {
    float v_max_sqr = INFINITY;
    float accel = INFINITY;
};

/** Highest squared speed through the corner from `prev` into `next`. */
float junctionMaxSqr(const Path& prev, const Path& next, float deviation);

}  // namespace planner

#endif
