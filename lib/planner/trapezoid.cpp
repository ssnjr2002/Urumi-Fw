#include "planner/trapezoid.h"

#include <math.h>

namespace planner {

static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

Trapezoid makeTrapezoid(float length, float accel, float v_entry_sqr,
                        float v_max_sqr, float v_exit_sqr) {
    Trapezoid tr;
    tr.length = length;
    tr.accel = accel;

    // Highest speed reachable from both ends: where the two ramps meet.
    const float peak_sqr = 0.5f * (v_entry_sqr + v_exit_sqr) + accel * length;
    const float cruise_sqr = fminf(v_max_sqr, peak_sqr);
    const float entry_sqr = fminf(v_entry_sqr, cruise_sqr);
    const float exit_sqr = fminf(v_exit_sqr, cruise_sqr);

    tr.v_entry = sqrtf(entry_sqr);
    tr.v_cruise = sqrtf(cruise_sqr);
    tr.v_exit = sqrtf(exit_sqr);

    tr.s_acc = (cruise_sqr - entry_sqr) / (2.0f * accel);
    tr.s_dec = (cruise_sqr - exit_sqr) / (2.0f * accel);
    float s_cruise = length - tr.s_acc - tr.s_dec;
    if (s_cruise < 0) {
        // Only reachable through rounding at a triangle's peak.
        tr.s_acc = clampf(tr.s_acc, 0, length);
        tr.s_dec = length - tr.s_acc;
        s_cruise = 0;
    }

    tr.t_acc = (tr.v_cruise - tr.v_entry) / accel;
    tr.t_dec = (tr.v_cruise - tr.v_exit) / accel;
    tr.t_cruise = tr.v_cruise > 0 ? s_cruise / tr.v_cruise : 0;
    return tr;
}

float Trapezoid::position(float t) const {
    if (t <= 0) return 0;
    if (t < t_acc) return v_entry * t + 0.5f * accel * t * t;
    t -= t_acc;
    if (t < t_cruise) return s_acc + v_cruise * t;
    t -= t_cruise;
    if (t >= t_dec) return length;
    const float s = length - s_dec + v_cruise * t - 0.5f * accel * t * t;
    return clampf(s, 0, length);
}

float Trapezoid::velocity(float t) const {
    if (t <= 0) return v_entry;
    if (t < t_acc) return v_entry + accel * t;
    t -= t_acc;
    if (t < t_cruise) return v_cruise;
    t -= t_cruise;
    if (t >= t_dec) return v_exit;
    return v_cruise - accel * t;
}

}  // namespace planner
