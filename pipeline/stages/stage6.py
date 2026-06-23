"""
Stage 6: Bezier evaluation -> MicroSegments (the geometry->step-events core).

Walks each PlannedCurve at adaptive dt driven by two constraints:
  1. Geometric:  dt <= sqrt(8 * CHORD_TOL / |B''(t)|)
  2. Velocity:   dt <= DV_MAX / (a_max * |B'(t)|)   [never change v by > DV_MAX per segment]
At each sample computes:
  dx, dy  — integer step deltas (steps_per_unit * position delta, rounded)
  da      — integer rotation steps (steps_per_deg * tangent angle delta)
  dz      — 0 (Z lift is choreography, see the lower half of this file)
  interval — RP2350 clock cycles for the major axis step
Output: list of MicroSegment(dx, dy, dz, da, interval, flags)

This module has two parts, kept separate function-wise:

  1. GEOMETRY CORE (evaluate_curve + dt/interval/tangent helpers) — turns one
     PlannedCurve into MicroSegments. Pure, deterministic; the parity spec the
     future C++ local-production port must reproduce.
  2. TOOL CHOREOGRAPHY (build_toolpath / evaluate_microsegments) — stitches the
     per-curve output into a whole job: travel jogs, Z pen-lift, and A-axis
     orientation, all selected by a ToolProfile. This is the outer loop that
     drives the core; it is not a separate pipeline stage.
"""

import math
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from stage4 import _bezier_deriv1, _bezier_deriv2, _bezier_point
from stage5 import PATH_START, PATH_END
from config import default as _config_default, ToolProfile, PEN
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

# ── velocity at arc length s ──────────────────────────────────────────────────

def _velocity_at_s(planned, s, length, a_max, q):
    """
    Trapezoidal velocity at arc-length position s along a curve of `length` mm.

    A real trapezoid in ARC LENGTH, not the curve parameter: accelerate from
    v_entry at a_max, cruise at v_cruise, decelerate to v_exit at a_max, taking
    only the physically-needed v²/2a distances. The old linear-in-t model
    decelerated over the whole second half of the curve regardless of distance,
    which on a long edge into a corner-stop crawled to a halt over many mm
    instead of the ~v²/2a it actually needs.
    """
    v_e, v_c, v_x = planned.v_entry, planned.v_cruise, planned.v_exit
    if length <= 1e-9:
        return max(v_c, q.v_min)
    s = min(max(s, 0.0), length)
    v_acc = math.sqrt(max(0.0, v_e * v_e + 2.0 * a_max * s))
    v_dec = math.sqrt(max(0.0, v_x * v_x + 2.0 * a_max * (length - s)))
    return max(min(v_c, v_acc, v_dec), q.v_min)

# ── adaptive dt ───────────────────────────────────────────────────────────────

def _dt_geom(c, t, q):
    """Geometry-based step limit: chord deviation < q.chord_tol."""
    d2 = _bezier_deriv2(c, t)
    mag2 = d2[0]**2 + d2[1]**2
    if mag2 < 1e-20:
        return q.dt_max
    return min(q.dt_max, math.sqrt(8 * q.chord_tol / math.sqrt(mag2)))

def _dt_vel(c, t, planned, q, s, length, a_max):
    """
    Velocity-based step limit: keep the speed change per segment under q.dv_max.
    Arc-length aware so the (now short) accel/decel ramps are sampled finely:
    dt <= dv_max / |dv/dt|, with dv/dt = (dv/ds)·(ds/dt). In a ramp |dv/ds| =
    a_max/v, and ds/dt = |B'(t)|, so dt <= dv_max·v / (a_max·|B'|).
    """
    d1 = _bezier_deriv1(c, t)
    speed = math.hypot(d1[0], d1[1])           # ds/dt
    if speed < 1e-9:
        return q.dt_max
    v = _velocity_at_s(planned, s, length, a_max, q)
    return min(q.dt_max, q.dv_max * v / (a_max * speed))

def _choose_dt(c, t, planned, q, s, length, a_max):
    return max(q.dt_min, min(_dt_geom(c, t, q),
                             _dt_vel(c, t, planned, q, s, length, a_max)))

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

    # Per-axis rate limit (Phase 2): the segment must be slow enough that no
    # axis exceeds its physical step-rate ceiling R_i = max_rate_i * spu_i.
    # |d_i| steps in time T -> rate |d_i|/T <= R_i  =>  T >= |d_i|/R_i.
    # max_rate_i == 0 means "unlimited" (axis skipped). This is what stops the A
    # axis demanding ~37 MHz on tight curves — the whole segment slows so the
    # knife stays within its slew rate (and XY slows with it).
    t_rate = 0.0
    for d, ax in ((dx, machine.x), (dy, machine.y), (dz, machine.z), (da, machine.a)):
        R = ax.max_rate * ax.steps_per_unit
        if R > 0 and d != 0:
            t_rate = max(t_rate, abs(d) / R)

    dist_mm = math.hypot(dx / x_spu, dy / y_spu)   # true XY tool distance (mm)
    if dist_mm < 1e-9:
        # pure rotation / Z move — no XY feed to govern; use the rate floor if any
        if t_rate > 0.0:
            cycles = t_rate / major * machine.f_cpu
            return max(1, min(int(cycles), machine.f_cpu))
        return _major_rate()

    seg_time = max(dist_mm / v, t_rate)         # feed time, floored by axis rates
    cycles = seg_time / major * machine.f_cpu  # per major-axis step
    return max(1, min(int(cycles), machine.f_cpu))

# ── single curve evaluator ────────────────────────────────────────────────────

def evaluate_curve(planned, machine, q, theta_current, pos_x, pos_y, is_last,
                   a_max, tangential=True, corner_angle_deg=360.0):
    """
    Evaluate one PlannedCurve into a list of MicroSegments.

    This is the geometry->step-events core (the parity spec). Path stitching,
    jogs, Z-lift and A choreography live in build_toolpath (below), which calls
    this per curve.
    Returns (segments, theta_current, pos_x, pos_y, cusps).
    pos_x/y are in steps (float accumulator, rounded at each segment).
    theta_current is in degrees (unwrapped).
    cusps is a list of (segment_index, pivot_true_steps): IN-CURVE corners where
    the tangent jumps by >= corner_angle_deg in a single step. At a near-cusp
    (e.g. the path spikes to a point and reverses) the position barely moves but
    the tangent swings up to 180deg; the chord-based dt does not subdivide there,
    so it would otherwise emit one giant da while the blade is DOWN (tearing the
    material). Instead the rotation is suppressed (da=0) and reported as a cusp,
    so build_toolpath inserts a lift-pivot-lower exactly as for between-curve
    corners. corner_angle_deg defaults to 360 (off) for non-knife callers.

    tangential — when True (drag-knife / creasing tool), the A axis tracks the
    path tangent (da). When False (pen / non-rotating tool), da = 0.
    """
    c = planned.metrics.curve
    segments = []
    cusps = []
    t = 0.0
    s = 0.0                                   # arc length walked (mm)
    length = planned.metrics.path_length_mm
    a_spu = machine.a.steps_per_unit
    a_accum = 0.0                             # cumulative true A (float steps);
                                              # round the running total and diff,
                                              # so per-segment da telescopes to
                                              # the exact net rotation (no bias).

    # step accumulators in mm (convert to steps at each segment)
    x_mm = c.p0[0]
    y_mm = c.p0[1]

    while t < 1.0:
        dt = _choose_dt(c, t, planned, q, s, length, a_max)
        t_next = min(t + dt, 1.0)

        p_next = _bezier_point(c, t_next)
        dx_mm = p_next[0] - x_mm
        dy_mm = p_next[1] - y_mm
        s_next = s + math.hypot(dx_mm, dy_mm)   # chord ~ arc length increment

        # integer steps via per-axis resolution; accumulate sub-step error in
        # the float position (TRUE geometry — invert is applied only to the
        # emitted delta sign, not the accumulator, so the shape stays correct).
        new_x = pos_x + dx_mm * machine.x.steps_per_unit
        new_y = pos_y + dy_mm * machine.y.steps_per_unit
        dx_steps = int(round(new_x)) - int(round(pos_x))
        dy_steps = int(round(new_y)) - int(round(pos_y))

        # tangent rotation (A axis) — only for tangential tools (knife/crease)
        theta_new = _tangent_angle(c, t_next)
        is_cusp = False
        da_steps = 0
        if tangential:
            delta_deg = _angle_delta(theta_current, theta_new)
            a_new  = a_accum + delta_deg * a_spu
            da_inc = int(round(a_new)) - int(round(a_accum))
            a_accum = a_new
            if abs(delta_deg) >= corner_angle_deg:
                # In-curve cusp: defer the turn to a lift-pivot (don't rotate in
                # material). Emit this XY step with da=0; report the pivot.
                is_cusp = True
                cusps.append((len(segments), da_inc))
            else:
                da_steps = da_inc

        # Sub-step sample (the arc-length ramp subdivides finely near v=0): no
        # axis actually steps. Accumulate into the float position and move on
        # rather than emit a zero-motion segment. (Keep cusps — they anchor a
        # pivot — and the final sample, which carries MICRO_PATH_END.)
        final = (t_next >= 1.0 and is_last)
        if not is_cusp and not final and dx_steps == 0 and dy_steps == 0 and da_steps == 0:
            pos_x, pos_y = new_x, new_y
            x_mm, y_mm = p_next[0], p_next[1]
            s = s_next
            theta_current = theta_new
            t = t_next
            continue

        # velocity and interval (geometry-aware so the tool honors v on diagonals)
        v = _velocity_at_s(planned, s_next, length, a_max, q)
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
        s     = s_next
        theta_current = theta_new
        t = t_next

    return segments, theta_current, pos_x, pos_y, cusps

# ══════════════════════════════════════════════════════════════════════════════
# TOOL CHOREOGRAPHY
# ──────────────────────────────────────────────────────────────────────────────
# Everything below stitches the per-curve step events above into a whole job:
# travel jogs between subpaths, Z pen/tool lift, and A-axis orientation for
# tangential tools. All tool-specific behaviour is selected by a ToolProfile
# (config.py), so adding a tool is a new preset, not new code. There is one
# knife model parameterized by offset_mm; a large offset would need offset
# compensation (PLAN P6, not implemented) and is rejected, not approximated.
# This is the outer loop that drives the geometry core — not a separate stage.
# ══════════════════════════════════════════════════════════════════════════════

def build_toolpath(planned_curves, machine, profile=None, quality=None,
                   jog_feed=None, lift_height=None, z_feed=None, a_max=None):
    """
    Stitch PlannedCurves into a flat list of MicroSegment, applying the tool's
    choreography.

    profile     — ToolProfile selecting tool behaviour. Defaults to PEN.
    quality     — QualityConfig. Defaults to config.default().quality.
    a_max       — acceleration mm/s² for the arc-length velocity ramp inside
                  each curve. Defaults to config.default().motion.a_max. Must
                  match the a_max stage5 planned with.
    jog_feed / lift_height / z_feed — explicit overrides; when None they fall
                  back to the profile, then to MotionConfig defaults.

    A travel (jog) MicroSegment moves the tool from the end of one path to the
    start of the next so paths land at their correct absolute positions; without
    it every closed subpath would be drawn relative to the previous endpoint
    (all stacked at the origin).
    """
    if profile is None:
        profile = PEN
    if profile.needs_offset_comp:
        raise NotImplementedError(
            f"tool profile '{profile.name}' has offset_mm={profile.offset_mm} "
            f"(> OFFSET_TOLERANCE_MM). Blade-offset compensation "
            "(XY_pivot = XY_cut - offset * tangent; PLAN_svg_tile_motion P6) is "
            "not implemented yet. Use a centre-pivot tool (offset_mm <= "
            "tolerance) until it lands."
        )

    _cfg = _config_default()
    q = quality if quality is not None else _cfg.quality
    if a_max is None:
        a_max = _cfg.motion.a_max
    tangential = profile.tangential
    corner_angle = profile.corner_angle_deg
    unwind = profile.unwind          # bounded-rotation tool: undo full turns pen-up
    a_inv = -1 if machine.a.invert else 1   # emitted-da sign -> true rotation

    # Resolve feeds: explicit arg > profile > MotionConfig default.
    if jog_feed is None:
        jog_feed = profile.jog_feed or _cfg.motion.jog_feed
    if lift_height is None:
        lift_height = profile.lift_height or _cfg.motion.lift_height
    if z_feed is None:
        z_feed = profile.z_feed or _cfg.motion.z_feed

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

    # A reorientation profile (pure rotation, pen-up). Cruise at the A ceiling
    # and accelerate at the A axis accel; fall back to sane defaults if unset.
    a_spd       = machine.a.steps_per_unit
    a_cruise_sps = max((machine.a.max_rate if machine.a.max_rate > 0 else 180.0) * a_spd, 1.0)
    a_accel_sps2 = max((machine.a.accel    if machine.a.accel    > 0 else 2000.0) * a_spd, 1.0)
    a_v0_sps     = min(a_cruise_sps, 50.0)   # gentle start, like the jog ramp

    def _a_move(da):
        """
        Pure-A rotation by `da` true steps as a RAMPED trapezoidal sequence of
        MicroSegments (accelerate from rest, cruise, decelerate) — NOT a single
        constant-rate segment. An unramped slam to the A ceiling stalls the
        stepper and drops steps, which accumulates as blade-angle drift over a
        path (worst at the last corner). Mirrors the host jog ramp. Flagged
        MICRO_JOG so validators exclude it from XY conservation checks; invert is
        applied to the emitted sign only.
        """
        N = abs(int(da))
        if N == 0:
            return []
        sign = (1 if da > 0 else -1) * (-1 if machine.a.invert else 1)
        v0, vc, acc = a_v0_sps, a_cruise_sps, a_accel_sps2

        d_acc = (vc**2 - v0**2) / (2.0 * acc)
        if 2 * d_acc > N:                       # triangular — never reach cruise
            d_acc = N / 2.0
        out = []
        n = 0
        while n < N:
            if n < d_acc:
                v = math.sqrt(v0**2 + 2.0 * acc * n)
            elif n >= N - d_acc:
                v = math.sqrt(max(v0**2, v0**2 + 2.0 * acc * (N - n)))
            else:
                v = vc
            v = max(v, v0)
            chunk = min(max(1, int(v / 100)), N - n)   # adaptive, ~10ms/segment
            iv = max(1, min(int(machine.f_cpu / v), machine.f_cpu))
            out.append(MicroSegment(dx=0, dy=0, dz=0, da=sign * chunk,
                                    interval=iv, flags=MICRO_JOG))
            n += chunk
        return out

    def _pivot(da_steps):
        """
        Reorient the tangential tool by da_steps. With lift available this is the
        lift-pivot-lower corner sequence (raise -> rotate -> lower) so the blade
        never rotates while embedded in the material — the tip would otherwise
        sweep a divot at a corner. Without a Z to lift, it degrades to an
        in-place rotation (the caller should bring XY to ~0 first; that v=0
        corner constraint is stage5's job). Returns the list of segments.
        """
        out = []
        if lift:
            out.append(_z_move(+z_steps))   # raise
        out.extend(_a_move(da_steps))       # pivot to new tangent (ramped)
        if lift:
            out.append(_z_move(-z_steps))   # lower
        return out

    all_segments = []
    theta = 0.0
    pos_x = 0.0
    pos_y = 0.0
    started = False
    a_phys = 0   # cumulative PHYSICAL A angle in true steps (sum of emitted da,
                 # un-inverted). Tracks wire wind-up so we can unwind it pen-up.

    for planned in planned_curves:
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

            # Orient the tangential tool to this path's entry tangent BEFORE
            # lowering. Covers both first-path pre-orientation (from rest at
            # theta=0) and between-path reorientation (from the previous path's
            # exit angle). Without this the planner's theta jumps to the new
            # entry while the physical blade stays put — it would tear the entry
            # of every path. Pen-up, so it runs between the jog and the lower.
            entry_theta = _tangent_angle(planned.metrics.curve, 0.0)
            if tangential:
                if unwind:
                    # Rotate to entry_theta's nearest-zero representative (its own
                    # value, in -180..180), unwinding any accumulated full turns:
                    # the full rotation a_phys -> target removes the wind-up while
                    # leaving the blade pointing along the entry tangent.
                    target   = int(round(entry_theta * a_spd))
                    da_steps = target - a_phys
                else:
                    da_steps = int(round(_angle_delta(theta, entry_theta) * a_spd))
                if da_steps != 0:
                    all_segments.extend(_a_move(da_steps))
                    a_phys += da_steps

            pos_x = target_x
            pos_y = target_y
            theta = entry_theta
            started = True

            if lift:
                all_segments.append(_z_move(-z_steps))  # lower pen to draw

        elif tangential:
            # Within-path corner: if the tangent jumps sharply between this curve
            # and the previous one, the blade would otherwise pivot while down
            # and tear an arc. Lift-pivot-lower instead. Smooth (C1) joins fall
            # below corner_angle and are tracked continuously by evaluate_curve.
            entry_theta = _tangent_angle(planned.metrics.curve, 0.0)
            jump = _angle_delta(theta, entry_theta)
            if abs(jump) >= corner_angle:
                da_steps = int(round(jump * a_spd))
                if da_steps != 0:
                    all_segments.extend(_pivot(da_steps))
                    a_phys += da_steps
                    theta = entry_theta   # blade is now at entry; no re-rotation

        is_last = bool(planned.flags & PATH_END)
        segs, theta, pos_x, pos_y, cusps = evaluate_curve(
            planned, machine, q, theta, pos_x, pos_y, is_last, a_max, tangential,
            corner_angle_deg=corner_angle if tangential else 360.0,
        )
        # Insert a lift-pivot-lower at each in-curve cusp evaluate_curve flagged
        # (the tangent reversed mid-curve; the rotation was deferred to here so
        # the blade doesn't pivot while embedded).
        if cusps:
            cusp_at = dict(cusps)
            for j, s in enumerate(segs):
                all_segments.append(s)
                if j in cusp_at:
                    all_segments.extend(_pivot(cusp_at[j]))
                    a_phys += cusp_at[j]
        else:
            all_segments.extend(segs)
        # Tracking rotation winds the physical A too (cusp segments carry da=0).
        a_phys += sum(s.da for s in segs) * a_inv

        if is_last and lift:
            all_segments.append(_z_move(+z_steps))  # raise pen after drawing

    return all_segments


def evaluate_microsegments(planned_curves, machine, quality=None, jog_feed=None,
                           lift_height=0.0, z_feed=None, tangential=True,
                           profile=None, a_max=None):
    """
    Backward-compatible entry point. Prefer build_toolpath() with a ToolProfile.

    When `profile` is given it drives behaviour. Otherwise the loose tangential/
    lift_height/z_feed args build an ad-hoc profile, preserving the old call
    sites (and the old default of a tangential tool with no lift). a_max should
    match the value stage5 planned with (defaults to MotionConfig.a_max).
    """
    if profile is None:
        profile = ToolProfile(name="adhoc", tangential=tangential)
    return build_toolpath(planned_curves, machine, profile=profile,
                          quality=quality, jog_feed=jog_feed,
                          lift_height=lift_height, z_feed=z_feed, a_max=a_max)


# ── standalone demo ─────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import argparse
    from stage2 import load_svg_mm
    from stage3 import enforce_c1
    from stage4 import compute_metrics
    from stage5 import plan_velocities
    from config import MachineConfig, TOOL_PROFILES

    cfg = _config_default()
    parser = argparse.ArgumentParser(description="Stage 6: SVG -> MicroSegments")
    parser.add_argument("svg",            help="Path to SVG file")
    parser.add_argument("--tool",         default="pen", choices=list(TOOL_PROFILES))
    parser.add_argument("--feed-max",     type=float, default=cfg.motion.feed_max)
    parser.add_argument("--a-max",        type=float, default=cfg.motion.a_max)
    parser.add_argument("--lift-height",  type=float, default=None)
    args = parser.parse_args()

    machine = cfg.machine
    profile = TOOL_PROFILES[args.tool]

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, _  = enforce_c1(curves_mm)
    metrics      = compute_metrics(repaired)
    flags_list   = [PATH_START] + [0] * (len(metrics) - 2) + [PATH_END]
    if len(metrics) == 1:
        flags_list = [PATH_START | PATH_END]

    planned  = plan_velocities(metrics, flags_list, args.feed_max, args.a_max)
    segments = build_toolpath(planned, machine, profile=profile,
                              quality=cfg.quality, lift_height=args.lift_height,
                              a_max=args.a_max)

    total_x = sum(s.dx for s in segments)
    total_y = sum(s.dy for s in segments)
    total_a = sum(s.da for s in segments)
    print(f"Tool          : {profile.name}")
    print(f"MicroSegments : {len(segments)}")
    print(f"Net steps     : dx={total_x}  dy={total_y}  da={total_a}")
    if segments:
        print(f"Interval range: {min(s.interval for s in segments)} - "
              f"{max(s.interval for s in segments)} cycles")
