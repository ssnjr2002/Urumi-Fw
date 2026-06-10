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
from config import default as _config_default
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
MICRO_LIFT     = 0x08   # pen/tool Z raise or lower (pen-up/down around a jog)

# ── constants ─────────────────────────────────────────────────────────────────

# Tuning constants live in pipeline/config.py (QualityConfig / MotionConfig).
# They are threaded in explicitly as `q` (QualityConfig) so each stage stays a
# pure function and standalone runs use config.default().

# ── velocity at parameter t ───────────────────────────────────────────────────

def _velocity_at_t(planned, t, q):
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
    return max(v, q.v_min)

# ── adaptive dt ───────────────────────────────────────────────────────────────

def _dt_geom(c, t, q):
    """Geometry-based step limit: chord deviation < q.chord_tol."""
    d2 = _bezier_deriv2(c, t)
    mag2 = d2[0]**2 + d2[1]**2
    if mag2 < 1e-20:
        return q.dt_max
    return min(q.dt_max, math.sqrt(8 * q.chord_tol / math.sqrt(mag2)))

def _dt_vel(c, t, planned, q):
    """
    Velocity-based step limit: speed change < q.dv_max per segment.
    Numerically differentiates the velocity profile — during cruise
    dv/dt ≈ 0 so dt falls back to q.dt_max; during accel/decel it
    subdivides finely.
    """
    eps = 1e-4
    v0 = _velocity_at_t(planned, t, q)
    v1 = _velocity_at_t(planned, min(t + eps, 1.0), q)
    dvdt = abs(v1 - v0) / eps
    if dvdt < 1e-6:
        return q.dt_max
    return min(q.dt_max, q.dv_max / dvdt)

def _choose_dt(c, t, planned, q):
    return max(q.dt_min, min(_dt_geom(c, t, q), _dt_vel(c, t, planned, q)))

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

def _interval(v, machine, q, dx=None, dy=None, dz=0, da=0):
    """
    Clock cycles per major-axis step so the XY TOOL moves at v mm/s.

    The Pico times a segment by its major axis (max steps over all driven axes),
    but the tool travels the XY hypotenuse — longer than the major leg on a
    diagonal. Without correction the realized tool speed overshoots v by up to
    sqrt(2) (a 45-degree move runs at v*sqrt(2)). Scaling the interval by
    hypot(dx,dy)/major restores the commanded feed; for a pure axis move
    hypot == major and it reduces to the plain major-axis rate.

    Called without dx/dy (legacy / non-geometric) it governs the major axis
    directly at v, matching the old behaviour.

    Per-axis: the XY tool distance is hypot(dx/x_spu, dy/y_spu), so X and Y may
    have different resolutions (non-square machine).
    """
    v = max(v, q.v_min)
    x_spu = machine.x.steps_per_unit
    y_spu = machine.y.steps_per_unit

    def _major_rate():
        step_rate = v * x_spu
        if step_rate < 1e-6:
            return machine.f_cpu
        return max(1, min(int(machine.f_cpu / step_rate), machine.f_cpu))

    if dx is None or dy is None:
        return _major_rate()

    major = max(abs(dx), abs(dy), abs(dz), abs(da))
    if major == 0:
        return machine.f_cpu
    dist_mm = math.hypot(dx / x_spu, dy / y_spu)   # true XY tool distance (mm)
    if dist_mm < 1e-9:
        # pure rotation / Z move — no XY to govern; time the major axis at v
        return _major_rate()

    seg_time = dist_mm / v                      # seconds for this segment
    cycles = seg_time / major * machine.f_cpu  # per major-axis step
    return max(1, min(int(cycles), machine.f_cpu))

# ── single curve evaluator ────────────────────────────────────────────────────

def _evaluate_curve(planned, machine, q, theta_current, pos_x, pos_y, is_last,
                    tangential=True):
    """
    Evaluate one PlannedCurve into a list of MicroSegments.
    Returns (segments, theta_current, pos_x, pos_y).
    pos_x/y are in steps (float accumulator, rounded at each segment).
    theta_current is in degrees (unwrapped).

    tangential — when True (drag-knife / creasing tool), the A axis tracks the
    path tangent (da). When False (pen / non-rotating tool), da = 0.
    """
    c = planned.metrics.curve
    segments = []
    t = 0.0

    # step accumulators in mm (convert to steps at each segment)
    x_mm = c.p0[0]
    y_mm = c.p0[1]

    while t < 1.0:
        dt = _choose_dt(c, t, planned, q)
        t_next = min(t + dt, 1.0)

        p_next = _bezier_point(c, t_next)
        dx_mm = p_next[0] - x_mm
        dy_mm = p_next[1] - y_mm

        # integer steps via per-axis resolution; accumulate sub-step error in
        # the float position (TRUE geometry — invert is applied only to the
        # emitted delta sign, not the accumulator, so the shape stays correct).
        new_x = pos_x + dx_mm * machine.x.steps_per_unit
        new_y = pos_y + dy_mm * machine.y.steps_per_unit
        dx_steps = int(round(new_x)) - int(round(pos_x))
        dy_steps = int(round(new_y)) - int(round(pos_y))

        # tangent rotation (A axis) — only for tangential tools (knife/crease)
        theta_new = _tangent_angle(c, t_next)
        if tangential:
            delta_deg = _angle_delta(theta_current, theta_new)
            da_steps  = int(round(delta_deg * machine.a.steps_per_unit))
        else:
            da_steps = 0

        # velocity and interval (geometry-aware so the tool honors v on diagonals)
        v = _velocity_at_t(planned, t_next, q)
        iv = _interval(v, machine, q, dx_steps, dy_steps, 0, da_steps)

        # flags
        f = MICRO_PATH_END if (t_next >= 1.0 and is_last) else 0

        segments.append(MicroSegment(
            dx=-dx_steps if machine.x.invert else dx_steps,
            dy=-dy_steps if machine.y.invert else dy_steps,
            dz=0,
            da=-da_steps if machine.a.invert else da_steps,
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

def evaluate_microsegments(planned_curves, machine, quality=None, jog_feed=None,
                           lift_height=0.0, z_feed=None, tangential=True):
    """
    Returns flat list of MicroSegment.

    quality     — QualityConfig (tuning constants). Defaults to config.default().
    jog_feed    — travel speed mm/s. Defaults to config.default().motion.jog_feed.
    lift_height — pen/tool Z lift between subpaths, mm. 0 (default) draws through
                  every jog (no Z motion). When > 0, the pen is raised after each
                  path, the XY jog runs pen-up, and the pen is lowered before the
                  next path. The first path lowers before drawing; the last path
                  raises at the end. Tool starts and ends pen-up.
    z_feed      — Z raise/lower speed mm/s. Defaults to config.default().motion.z_feed.

    Between subpaths a travel (jog) MicroSegment moves the tool from the end of
    one path to the start of the next, so paths land at their correct absolute
    positions. Without it, every closed subpath would be drawn relative to the
    previous path's endpoint (all stacked at the origin).
    """
    _cfg = _config_default()
    q = quality if quality is not None else _cfg.quality
    if jog_feed is None:
        jog_feed = _cfg.motion.jog_feed
    if z_feed is None:
        z_feed = _cfg.motion.z_feed

    # Pre-compute the Z lift move (pure Z, timed by z_feed on the Z axis)
    lift = lift_height > 0.0
    z_steps = int(round(lift_height * machine.z.steps_per_unit)) if lift else 0
    if z_steps == 0:
        lift = False
    z_rate = max(z_feed * machine.z.steps_per_unit, 1e-9)
    z_interval = max(1, min(int(machine.f_cpu / z_rate), machine.f_cpu))

    def _z_move(dz):
        # dz > 0 raises (pen up), dz < 0 lowers (pen down) — see MicroSegment.dz.
        # z.invert flips the emitted sign for a Z wired opposite the convention.
        return MicroSegment(dx=0, dy=0,
                            dz=(-dz if machine.z.invert else dz), da=0,
                            interval=z_interval, flags=MICRO_LIFT)

    all_segments = []
    theta = 0.0
    pos_x = 0.0
    pos_y = 0.0
    started = False

    for i, planned in enumerate(planned_curves):
        # reset position and angle at each PATH_START
        if planned.flags & PATH_START:
            p0 = planned.metrics.curve.p0
            target_x = p0[0] * machine.x.steps_per_unit
            target_y = p0[1] * machine.y.steps_per_unit

            # Insert a travel move from the previous path's end to this start.
            # jog_dx/dy are TRUE geometry; invert applies only to emitted sign.
            if started:
                jog_dx = int(round(target_x)) - int(round(pos_x))
                jog_dy = int(round(target_y)) - int(round(pos_y))
                if jog_dx != 0 or jog_dy != 0:
                    all_segments.append(MicroSegment(
                        dx=-jog_dx if machine.x.invert else jog_dx,
                        dy=-jog_dy if machine.y.invert else jog_dy,
                        dz=0, da=0,
                        interval=_interval(jog_feed, machine, q, jog_dx, jog_dy),
                        flags=MICRO_JOG,
                    ))

            pos_x = target_x
            pos_y = target_y
            theta = _tangent_angle(planned.metrics.curve, 0.0)
            started = True

            if lift:
                all_segments.append(_z_move(-z_steps))  # lower pen to draw

        is_last = bool(planned.flags & PATH_END)
        segs, theta, pos_x, pos_y = _evaluate_curve(
            planned, machine, q, theta, pos_x, pos_y, is_last, tangential
        )
        all_segments.extend(segs)

        if is_last and lift:
            all_segments.append(_z_move(+z_steps))  # raise pen after drawing

    return all_segments

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    from config import default as _default, MachineConfig
    cfg = _default()

    parser = argparse.ArgumentParser(description="Stage 6: Bezier -> MicroSegments")
    parser.add_argument("svg",            help="Path to SVG file")
    parser.add_argument("--feed-max",     type=float, default=cfg.motion.feed_max)
    parser.add_argument("--a-max",        type=float, default=cfg.motion.a_max)
    parser.add_argument("--steps-per-mm", type=float, default=cfg.machine.steps_per_mm)
    parser.add_argument("--steps-per-deg",type=float, default=cfg.machine.steps_per_deg)
    parser.add_argument("--f-cpu",        type=int,   default=cfg.machine.f_cpu)
    args = parser.parse_args()

    machine = MachineConfig.uniform(args.steps_per_mm, args.steps_per_deg, args.f_cpu)

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, _  = enforce_c1(curves_mm)
    metrics      = compute_metrics(repaired)
    flags_list   = [PATH_START] + [0] * (len(metrics) - 2) + [PATH_END]
    if len(metrics) == 1:
        flags_list = [PATH_START | PATH_END]

    planned  = plan_velocities(metrics, flags_list, args.feed_max, args.a_max)
    segments = evaluate_microsegments(planned, machine, quality=cfg.quality)

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
