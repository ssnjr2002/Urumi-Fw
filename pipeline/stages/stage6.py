"""
Stage 6: Bezier evaluation -> MicroSegments.
Walks each PlannedCurve at adaptive dt driven by two constraints:
  1. Geometric:  dt <= sqrt(8 * CHORD_TOL / |B''(t)|)
  2. Velocity:   dt <= DV_MAX / (a_max * |B'(t)|)   [never change v by > DV_MAX per segment]
At each sample computes:
  dx, dy  — integer step deltas (steps_per_mm * position delta, rounded)
  da      — integer rotation steps (steps_per_deg * tangent angle delta)
  dz      — 0 for now (Z lift scheduled separately in a later stage)
  interval — RP2350 clock cycles for the major axis step
Output: list of MicroSegment(dx, dy, dz, da, interval, flags)
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from stage1 import CubicBezier
from stage2 import load_svg_mm
from stage3 import enforce_c1
from stage4 import compute_metrics, _bezier_deriv1, _bezier_deriv2, _bezier_point
from stage5 import plan_velocities, PATH_START, PATH_END
from collections import namedtuple

# ── output structure ──────────────────────────────────────────────────────────

MicroSegment = namedtuple("MicroSegment", [
    "dx",        # X steps (signed int)
    "dy",        # Y steps (signed int)
    "dz",        # Z steps (signed int, 0 until lift stage)
    "da",        # A steps (signed int, tangential rotation)
    "interval",  # clock cycles for major axis
    "flags",     # MICRO_PATH_END etc.
])

MICRO_PATH_END = 0x01
MICRO_JOG      = 0x04   # travel move between subpaths (0x02 reserved for ESTOP)

# ── constants ─────────────────────────────────────────────────────────────────

CHORD_TOL  = 0.01   # mm — max chord deviation per segment
DV_MAX     = 3.0    # mm/s — max velocity change per segment
V_MIN      = 0.5    # mm/s — floor to avoid divide-by-zero
DT_MAX     = 0.05   # max parameter step (never skip >5% of curve at once)
DT_MIN     = 1e-6   # guard against infinite loops
JOG_FEED   = 80.0   # mm/s — travel speed between subpaths (pen-up rapid)

# ── velocity at parameter t ───────────────────────────────────────────────────

def _velocity_at_t(planned, t):
    """
    Linearly interpolate velocity along arc length parameter.
    We approximate arc-length fraction ≈ t (close enough for interval calc).
    """
    v_e, v_c, v_x = planned.v_entry, planned.v_cruise, planned.v_exit
    # ramp up then ramp down: use t as proxy for arc-length fraction
    if t <= 0.5:
        v = v_e + (v_c - v_e) * (t * 2)
    else:
        v = v_c + (v_x - v_c) * ((t - 0.5) * 2)
    return max(v, V_MIN)

# ── adaptive dt ───────────────────────────────────────────────────────────────

def _dt_geom(c, t):
    """Geometry-based step limit: chord deviation < CHORD_TOL."""
    d2 = _bezier_deriv2(c, t)
    mag2 = d2[0]**2 + d2[1]**2
    if mag2 < 1e-20:
        return DT_MAX
    return min(DT_MAX, math.sqrt(8 * CHORD_TOL / math.sqrt(mag2)))

def _dt_vel(c, t, planned):
    """
    Velocity-based step limit: speed change < DV_MAX per segment.
    Numerically differentiates the velocity profile — during cruise
    dv/dt ≈ 0 so dt falls back to DT_MAX; during accel/decel it
    subdivides finely.
    """
    eps = 1e-4
    v0 = _velocity_at_t(planned, t)
    v1 = _velocity_at_t(planned, min(t + eps, 1.0))
    dvdt = abs(v1 - v0) / eps
    if dvdt < 1e-6:
        return DT_MAX
    return min(DT_MAX, DV_MAX / dvdt)

def _choose_dt(c, t, planned):
    return max(DT_MIN, min(_dt_geom(c, t), _dt_vel(c, t, planned)))

# ── angle helpers ─────────────────────────────────────────────────────────────

def _tangent_angle(c, t):
    """Tangent angle in degrees at parameter t."""
    d1 = _bezier_deriv1(c, t)
    if d1[0]**2 + d1[1]**2 < 1e-20:
        return 0.0
    return math.degrees(math.atan2(d1[1], d1[0]))

def _angle_delta(a, b):
    """Shortest signed rotation from angle a to angle b (degrees, range ±180)."""
    d = b - a
    while d >  180: d -= 360
    while d < -180: d += 360
    return d

# ── interval ──────────────────────────────────────────────────────────────────

def _interval(v, machine):
    """Clock cycles for one major-axis step at velocity v (mm/s)."""
    v = max(v, V_MIN)
    step_rate = v * machine.steps_per_mm   # steps/sec
    if step_rate < 1e-6:
        return machine.f_cpu              # saturate at 1 step/sec
    cycles = machine.f_cpu / step_rate
    return min(int(cycles), machine.f_cpu)  # cap at 1 step/sec

# ── single curve evaluator ────────────────────────────────────────────────────

def _evaluate_curve(planned, machine, theta_current, pos_x, pos_y, is_last):
    """
    Evaluate one PlannedCurve into a list of MicroSegments.
    Returns (segments, theta_current, pos_x, pos_y).
    pos_x/y are in steps (float accumulator, rounded at each segment).
    theta_current is in degrees (unwrapped).
    """
    c = planned.metrics.curve
    segments = []
    t = 0.0

    # step accumulators in mm (convert to steps at each segment)
    x_mm = c.p0[0]
    y_mm = c.p0[1]

    while t < 1.0:
        dt = _choose_dt(c, t, planned)
        t_next = min(t + dt, 1.0)

        p_next = _bezier_point(c, t_next)
        dx_mm = p_next[0] - x_mm
        dy_mm = p_next[1] - y_mm

        # integer steps: round to nearest, accumulate sub-step error via float pos
        new_x = pos_x + dx_mm * machine.steps_per_mm
        new_y = pos_y + dy_mm * machine.steps_per_mm
        dx_steps = int(round(new_x)) - int(round(pos_x))
        dy_steps = int(round(new_y)) - int(round(pos_y))

        # tangent rotation
        theta_new = _tangent_angle(c, t_next)
        delta_deg = _angle_delta(theta_current, theta_new)
        da_steps  = int(round(delta_deg * machine.steps_per_deg))

        # velocity and interval
        v = _velocity_at_t(planned, t_next)
        iv = _interval(v, machine)

        # flags
        f = MICRO_PATH_END if (t_next >= 1.0 and is_last) else 0

        segments.append(MicroSegment(
            dx=dx_steps, dy=dy_steps, dz=0, da=da_steps,
            interval=iv, flags=f,
        ))

        pos_x = new_x
        pos_y = new_y
        x_mm  = p_next[0]
        y_mm  = p_next[1]
        theta_current = theta_new
        t = t_next

    return segments, theta_current, pos_x, pos_y

# ── main stage ────────────────────────────────────────────────────────────────

def evaluate_microsegments(planned_curves, machine, jog_feed=JOG_FEED):
    """
    Returns flat list of MicroSegment.

    Between subpaths a travel (jog) MicroSegment is inserted to move the tool
    from the end of one path to the start of the next, so paths land at their
    correct absolute positions. Without it, every closed subpath would be drawn
    relative to the previous path's endpoint (all stacked at the origin).

    The jog is a single constant-velocity move at jog_feed — pen-lift during
    travel and jog acceleration ramping are separate concerns (Z axis / future).
    """
    all_segments = []
    theta = 0.0
    pos_x = 0.0
    pos_y = 0.0
    started = False

    for i, planned in enumerate(planned_curves):
        # reset position and angle at each PATH_START
        if planned.flags & PATH_START:
            p0 = planned.metrics.curve.p0
            target_x = p0[0] * machine.steps_per_mm
            target_y = p0[1] * machine.steps_per_mm

            # Insert a travel move from the previous path's end to this start
            if started:
                jog_dx = int(round(target_x)) - int(round(pos_x))
                jog_dy = int(round(target_y)) - int(round(pos_y))
                if jog_dx != 0 or jog_dy != 0:
                    all_segments.append(MicroSegment(
                        dx=jog_dx, dy=jog_dy, dz=0, da=0,
                        interval=_interval(jog_feed, machine), flags=MICRO_JOG,
                    ))

            pos_x = target_x
            pos_y = target_y
            theta = _tangent_angle(planned.metrics.curve, 0.0)
            started = True

        is_last = bool(planned.flags & PATH_END)
        segs, theta, pos_x, pos_y = _evaluate_curve(
            planned, machine, theta, pos_x, pos_y, is_last
        )
        all_segments.extend(segs)

    return all_segments

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Stage 6: Bezier -> MicroSegments")
    parser.add_argument("svg",            help="Path to SVG file")
    parser.add_argument("--feed-max",     type=float, default=80.0)
    parser.add_argument("--a-max",        type=float, default=1000.0)
    parser.add_argument("--steps-per-mm", type=float, default=80.0)
    parser.add_argument("--steps-per-deg",type=float, default=10.0)
    parser.add_argument("--f-cpu",        type=int,   default=150_000_000)
    args = parser.parse_args()

    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))
    from mock_stage6 import MachineConfig
    machine = MachineConfig(args.steps_per_mm, args.steps_per_deg, args.f_cpu)

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, _  = enforce_c1(curves_mm)
    metrics      = compute_metrics(repaired)
    flags_list   = [PATH_START] + [0] * (len(metrics) - 2) + [PATH_END]
    if len(metrics) == 1:
        flags_list = [PATH_START | PATH_END]

    planned  = plan_velocities(metrics, flags_list, args.feed_max, args.a_max)
    segments = evaluate_microsegments(planned, machine)

    total_x = sum(s.dx for s in segments)
    total_y = sum(s.dy for s in segments)
    total_a = sum(s.da for s in segments)
    print(f"MicroSegments : {len(segments)}")
    print(f"Net steps     : dx={total_x}  dy={total_y}  da={total_a}")
    if segments:
        print(f"Interval range: {min(s.interval for s in segments)} - "
              f"{max(s.interval for s in segments)} cycles")
    else:
        print("No segments generated (no path elements?)")
    print()
    print(f"  {'#':>5}  {'dx':>6}  {'dy':>6}  {'da':>6}  {'interval':>10}  flags")
    print(f"  {'-'*5}  {'-'*6}  {'-'*6}  {'-'*6}  {'-'*10}  -----")
    for i, s in enumerate(segments[:40]):
        print(f"  {i:5d}  {s.dx:6d}  {s.dy:6d}  {s.da:6d}  {s.interval:10d}  {s.flags}")
    if len(segments) > 40:
        print(f"  ... ({len(segments) - 40} more)")
