/**
 * path.h — what look-ahead and junctions need from any block, line or Bézier.
 *
 * Corners use junction deviation (GRBL) on the end tangents: the speed at which
 * a circle of radius set by `deviation` could round the corner at the lower of
 * the two accelerations. A tangent-continuous join costs nothing; only the
 * neighbours' own caps limit it.
 *
 * A path moves one axis set: XY (A may ride along), Z alone or A alone. Joins
 * between different sets stop. A Z or A path's direction is {±1, 0}, so the
 * same formula runs straight on at full speed and stops on a reversal.
 *
 * A riding along XY turns `a_rate` rad per mm of path at each end: κ on a
 * tangential Bézier, ΔA/length on a line, 0 where A stays. Where the rates
 * differ at a join A's speed steps by v·Δrate; the join is capped so the step
 * stays under `a_jump`, the step that lags the blade at most kAMisalignRad.
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

constexpr float kPiF = 3.14159265f;
constexpr float kRadPerDeg = kPiF / 180.0f;
constexpr float kAMisalignRad = 2.0f * kRadPerDeg;   // e_max; the bench tunes it
constexpr float kAAccelShare = 0.5f;                 // of A's accel left for v²·dκ/ds

/** The axis set a path moves; XY with A riding along is XY. */
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
    float a_rate_start = 0;   // XY: rad of A per mm of path, signed
    float a_rate_end = 0;
    float a_jump = INFINITY;  // XY: rad/s A's speed may step at a join
};

/** Caps a caller adds to a path beyond its axes: another axis riding along. */
struct PathCap {
    float v_max_sqr = INFINITY;
    float accel = INFINITY;
};

/** Highest squared speed through the corner from `prev` into `next`. */
float junctionMaxSqr(const Path& prev, const Path& next, float deviation);

/** √(2·α·e_max): A's speed step at a join, rad/s, from A's limits. */
float aJump(const AxisLimits& limits);

}  // namespace planner

#endif
