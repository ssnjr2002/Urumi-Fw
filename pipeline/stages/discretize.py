"""
Discretize stage (redesign stages 7+8): Sample stream -> MicroSegments.

This is the bottom of the redesigned pipeline: it consumes the planned Sample
stream (positions + tangents + resolved speed v) and emits the wire-level step
events, applying the tool's choreography on the way down.

Two jobs, kept separate function-wise (mirroring how today's stage6 splits its
geometry core from its choreography):

  CHOREOGRAPH (stage 7) — the outer walk. Inserts non-cutting motion around the
    cut: a travel jog between subpaths, A-axis pre-orientation + Z-lower at each
    PATH_START, lift-pivot-lower at corners, Z-raise at PATH_END, and bounded A
    unwind for a wired tool. Driven entirely by a ToolProfile.
  DISCRETIZE (stage 8) — the per-pair emit. Turns each consecutive sample pair
    into one MicroSegment: per-axis integer step deltas (float accumulators),
    tangent-tracking da, and an interval from the planned speed.

Why this is simpler than the tile-era stage6: velocity planning already brought
the tool to v=0 at every corner (Constrain set v_ceiling=0, Plan propagated it),
so a corner is just "two adjacent samples whose tangent jumps by >= the tool's
corner angle". Between-curve corners and in-curve cusps collapse into ONE rule —
no separate cusp scan, no per-curve corner bookkeeping.

Parity: the per-axis interval, invert, and float-accumulator conventions match
stage6 exactly (the helpers are imported from it), so the emitted steps are the
same kind the working pipeline produces.
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from config import default as _config_default, ToolProfile, PEN
from sample import Sample, PATH_START, PATH_END, CURVE_BOUNDARY
from plan_lookahead import _subpath_ranges
from microsegment import (MicroSegment, interval as _interval,
                          angle_delta as _angle_delta,
                          MICRO_PATH_END, MICRO_JOG, MICRO_LIFT)


def discretize(samples, machine, profile=None, quality=None, a_max=None,
               jog_feed=None, lift_height=None, z_feed=None):
    """
    Walk the planned Sample stream and emit a flat list[MicroSegment].

    samples — list[Sample] with v resolved (after Constrain + Plan).
    profile — ToolProfile (PEN/KNIFE/CREASE); defaults to PEN. Selects tangent
              tracking, corner threshold, unwind, lift.
    a_max   — kept for signature parity with build_toolpath; the interval comes
              from the per-sample v, so a_max is unused here (the ramp is already
              baked into v by the Plan stage). Accepted and ignored.
    jog_feed / lift_height / z_feed — explicit overrides; None falls back to the
              profile, then to MachineConfig defaults (jog_feed, z_feed).
    """
    if profile is None:
        profile = PEN
    if profile.needs_offset_comp:
        raise NotImplementedError(
            f"tool profile '{profile.name}' has offset_mm={profile.offset_mm} "
            "(> OFFSET_TOLERANCE_MM); blade-offset compensation (PLAN P6) is not "
            "implemented. Use a centre-pivot tool until it lands."
        )

    _cfg = _config_default()
    q = quality if quality is not None else _cfg.quality
    tangential   = profile.tangential
    corner_angle = profile.corner_angle_deg
    unwind       = profile.unwind

    if jog_feed is None:
        jog_feed = profile.jog_feed or machine.jog_feed
    if lift_height is None:
        lift_height = profile.lift_height          # 0 = draw-through, no fallback
    if z_feed is None:
        z_feed = profile.z_feed or machine.z_feed

    x_spu = machine.x.steps_per_unit
    y_spu = machine.y.steps_per_unit
    a_spd = machine.a.steps_per_unit

    # ── Z lift move (pure Z, timed by z_feed) ─────────────────────────────────
    z_steps = int(round(lift_height * machine.z.steps_per_unit)) if lift_height > 0 else 0
    lift = z_steps > 0
    z_rate = max(z_feed * machine.z.steps_per_unit, 1e-9)
    z_interval = max(1, min(int(machine.f_cpu / z_rate), machine.f_cpu))

    def _z_move(dz):
        return MicroSegment(dx=0, dy=0,
                            dz=(-dz if machine.z.invert else dz), da=0,
                            interval=z_interval, flags=MICRO_LIFT)

    # ── ramped pure-A rotation (trapezoidal; never slam the A axis) ────────────
    a_cruise_sps = max((machine.a.max_rate if machine.a.max_rate > 0 else 180.0) * a_spd, 1.0)
    a_accel_sps2 = max((machine.a.accel    if machine.a.accel    > 0 else 2000.0) * a_spd, 1.0)
    a_v0_sps     = min(a_cruise_sps, 50.0)

    def _a_move(da):
        N = abs(int(da))
        if N == 0:
            return []
        sign = (1 if da > 0 else -1) * (-1 if machine.a.invert else 1)
        v0, vc, acc = a_v0_sps, a_cruise_sps, a_accel_sps2
        d_acc = (vc**2 - v0**2) / (2.0 * acc)
        if 2 * d_acc > N:
            d_acc = N / 2.0
        out, n = [], 0
        while n < N:
            if n < d_acc:
                v = math.sqrt(v0**2 + 2.0 * acc * n)
            elif n >= N - d_acc:
                v = math.sqrt(max(v0**2, v0**2 + 2.0 * acc * (N - n)))
            else:
                v = vc
            v = max(v, v0)
            chunk = min(max(1, int(v / 100)), N - n)
            iv = max(1, min(int(machine.f_cpu / v), machine.f_cpu))
            out.append(MicroSegment(dx=0, dy=0, dz=0, da=sign * chunk,
                                    interval=iv, flags=MICRO_JOG))
            n += chunk
        return out

    def _pivot(da_true):
        """Lift-pivot-lower: raise -> rotate by da_true -> lower (Z optional)."""
        out = []
        if lift:
            out.append(_z_move(+z_steps))
        out.extend(_a_move(da_true))
        if lift:
            out.append(_z_move(-z_steps))
        return out

    out = []
    pos_x = pos_y = 0.0     # step accumulators (float; round at emit)
    theta = 0.0             # logical current tangent (deg)
    a_accum = 0.0           # float A steps (tracking; telescopes to exact net)
    a_phys = 0              # physical A steps (TRUE rotation; for unwind)
    started = False

    for lo, hi in _subpath_ranges(samples):
        first = samples[lo]
        target_x = first.x * x_spu
        target_y = first.y * y_spu

        # travel jog from previous subpath's end
        if started:
            jog_dx = int(round(target_x)) - int(round(pos_x))
            jog_dy = int(round(target_y)) - int(round(pos_y))
            if jog_dx or jog_dy:
                out.append(MicroSegment(
                    dx=-jog_dx if machine.x.invert else jog_dx,
                    dy=-jog_dy if machine.y.invert else jog_dy,
                    dz=0, da=0,
                    interval=_interval(jog_feed, machine, q, jog_dx, jog_dy),
                    flags=MICRO_JOG))

        # A pre-orientation to the entry tangent (pen-up), incl. unwind
        entry_theta = first.theta
        if tangential:
            if unwind:
                target = int(round(entry_theta * a_spd))
                da_true = target - a_phys
            else:
                da_true = int(round(_angle_delta(theta, entry_theta) * a_spd))
            if da_true:
                out.extend(_a_move(da_true))
                a_phys += da_true

        pos_x, pos_y = target_x, target_y
        theta = entry_theta
        a_accum = float(a_phys)
        started = True

        if lift:
            out.append(_z_move(-z_steps))   # lower to cut

        # ── walk the cutting samples ──────────────────────────────────────────
        for i in range(lo, hi):
            a, b = samples[i], samples[i + 1]
            dtheta = _angle_delta(theta, b.theta)
            is_corner = tangential and abs(dtheta) >= corner_angle
            final = (i + 1 == hi)

            # Velocity-aware subdivision (premortem P3): a flattened sample can be
            # ~1mm long; at the machine's step resolution one constant-interval
            # segment that long would change speed in a single jerky leap through
            # an accel zone. Split the pair into k sub-segments so the speed never
            # changes by more than q.dv_max within one MicroSegment. Cruise (dv~0)
            # stays k=1; only ramps subdivide. Corners (v~0 both ends, dtheta huge)
            # also stay k=1 — handled below as a single near-zero step + pivot.
            if is_corner:
                k = 1
            else:
                k = max(1, math.ceil(abs(b.v - a.v) / q.dv_max))
                k = min(k, 256)

            base_x, base_y = pos_x, pos_y     # float step accumulators at pair start
            for j in range(1, k + 1):
                f = j / k
                tgt_x = base_x + (b.x - a.x) * x_spu * f
                tgt_y = base_y + (b.y - a.y) * y_spu * f
                dx = int(round(tgt_x)) - int(round(pos_x))
                dy = int(round(tgt_y)) - int(round(pos_y))

                th_f = theta + dtheta * f
                da = 0
                if tangential and not is_corner:
                    a_new = a_accum + (th_f - (theta if j == 1 else th_prev)) * a_spd
                    da = int(round(a_new)) - int(round(a_accum))
                    a_accum = a_new
                th_prev = th_f

                last_sub = (j == k)
                seg_final = final and last_sub

                # skip zero-motion interior sub-steps (don't drop the final/corner)
                if not seg_final and not (is_corner and last_sub) \
                        and dx == 0 and dy == 0 and da == 0:
                    pos_x, pos_y = tgt_x, tgt_y
                    continue

                v0 = a.v + (b.v - a.v) * ((j - 1) / k)
                v1 = a.v + (b.v - a.v) * f
                vbar = 0.5 * (v0 + v1)
                iv = _interval(vbar, machine, q, dx, dy, 0, da)
                flags = MICRO_PATH_END if seg_final else 0
                out.append(MicroSegment(
                    dx=-dx if machine.x.invert else dx,
                    dy=-dy if machine.y.invert else dy,
                    dz=0,
                    da=-da if machine.a.invert else da,
                    interval=iv, flags=flags))
                a_phys += da
                pos_x, pos_y = tgt_x, tgt_y

            theta = b.theta

            # lift-pivot-lower at the corner we just arrived at (v is ~0 here)
            if is_corner:
                da_true = int(round(dtheta * a_spd))
                if da_true:
                    out.extend(_pivot(da_true))
                    a_phys += da_true
                a_accum = float(a_phys)

        if lift:
            out.append(_z_move(+z_steps))   # raise after the stroke

    return out


# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    from stage2 import load_svg_mm_subpaths
    from stage3 import enforce_c1
    from flatten import flatten
    from constrain import constrain
    from plan_lookahead import plan
    from config import TOOL_PROFILES

    cfg = _config_default()
    parser = argparse.ArgumentParser(description="Discretize: SVG -> MicroSegments (redesign)")
    parser.add_argument("svg", help="Path to SVG file")
    parser.add_argument("--tool",        default="knife", choices=list(TOOL_PROFILES))
    parser.add_argument("--feed-max",    type=float, default=None,
                        help="Cut feed mm/s (default: selected tool's feed_max)")
    parser.add_argument("--a-max",       type=float, default=cfg.machine.x.accel)
    parser.add_argument("--lift-height", type=float, default=None)
    args = parser.parse_args()

    machine = cfg.machine
    profile = TOOL_PROFILES[args.tool]
    feed_max = args.feed_max if args.feed_max is not None else profile.feed_max
    a_rate  = machine.a.max_rate if profile.tangential else 0.0
    a_accel = machine.a.accel    if profile.tangential else 0.0
    corner  = profile.corner_angle_deg if profile.tangential else None

    subpaths_mm, _ = load_svg_mm_subpaths(args.svg)
    repaired = [enforce_c1(sp)[0] for sp in subpaths_mm]
    samples  = flatten(repaired, quality=cfg.quality)
    constrain(samples, feed_max, args.a_max, a_rate_deg_s=a_rate,
              a_accel_deg_s2=a_accel, corner_stop_angle_deg=corner)
    plan(samples, machine, a_max=args.a_max)
    segs = discretize(samples, machine, profile=profile, quality=cfg.quality,
                      lift_height=args.lift_height)

    tx = sum(s.dx for s in segs); ty = sum(s.dy for s in segs); ta = sum(s.da for s in segs)
    print(f"Tool          : {profile.name}")
    print(f"MicroSegments : {len(segs)}")
    print(f"Net steps     : dx={tx}  dy={ty}  da={ta}")
    if segs:
        print(f"Interval range: {min(s.interval for s in segs)} - "
              f"{max(s.interval for s in segs)} cycles")
