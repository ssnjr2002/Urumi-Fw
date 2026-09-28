/**
 * trapezoid.h — one block's speed profile: accelerate, cruise, decelerate.
 *
 * Speeds are passed squared, as the look-ahead produces them. A block too short
 * to reach its cruise speed becomes a triangle whose peak is where the accel
 * and decel ramps meet. Distance along the block is closed-form in time
 * (seed §12), so evaluation never accumulates error.
 */

#ifndef PLANNER_TRAPEZOID_H
#define PLANNER_TRAPEZOID_H

namespace planner {

struct Trapezoid {
    float length = 0;     // mm
    float accel = 0;      // mm/s², used for both ramps
    float v_entry = 0;    // mm/s
    float v_cruise = 0;
    float v_exit = 0;
    float t_acc = 0;      // s
    float t_cruise = 0;
    float t_dec = 0;
    float s_acc = 0;      // mm covered by each ramp
    float s_dec = 0;

    float duration() const { return t_acc + t_cruise + t_dec; }
    float position(float t) const;   // mm along the block, clamped to [0, length]
    float velocity(float t) const;   // mm/s
};

/**
 * Build a profile. Expects a feasible request, as the look-ahead guarantees:
 * `|v_entry² − v_exit²| ≤ 2·accel·length` and both ends ≤ `v_max²`. Float
 * rounding past those bounds is clamped rather than propagated.
 */
Trapezoid makeTrapezoid(float length, float accel, float v_entry_sqr,
                        float v_max_sqr, float v_exit_sqr);

}  // namespace planner

#endif
