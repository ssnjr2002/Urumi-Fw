"""
Stage 5: Velocity planner — forward + backward trapezoidal pass.
Input:  list of CurveMetrics + parallel flags list + feed_max + a_max
Output: list of PlannedCurve, each annotated with v_entry, v_cruise, v_exit (mm/s)

Flags (from mock_stage5):
  PATH_START      = 0x01  → v_entry forced to 0
  PATH_END        = 0x02  → v_exit  forced to 0
  MERGE_WITH_PREV = 0x04  → curve shares velocity envelope with previous

Velocity caps (lowest wins):
  1. feed_max          — tool config cruise limit
  2. centripetal limit — sqrt(a_max / kappa_max), skipped when kappa_max == 0
  3. reachable speed   — what forward/backward pass can actually reach
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from stage1 import CubicBezier
from stage2 import load_svg_mm
from stage3 import enforce_c1
from stage4 import compute_metrics
from config import default as _config_default
from collections import namedtuple

# ── flags (mirrors mock_stage5) ───────────────────────────────────────────────

PATH_START      = 0x01
PATH_END        = 0x02
MERGE_WITH_PREV = 0x04

# ── output structure ──────────────────────────────────────────────────────────

PlannedCurve = namedtuple("PlannedCurve", [
    "metrics",      # original CurveMetrics
    "flags",        # int flag set
    "v_entry",      # mm/s at curve start
    "v_cruise",     # mm/s peak (may never be reached on short/triangular curves)
    "v_exit",       # mm/s at curve end
    "merged",       # bool — part of a merged group
])

# ── helpers ───────────────────────────────────────────────────────────────────

def _v_reachable(v_from, a_max, length):
    """Max speed reachable from v_from over a given distance."""
    return math.sqrt(max(0.0, v_from**2 + 2 * a_max * length))

def _v_cruise_cap(m, feed_max, a_max, a_rate_deg_s=0.0):
    """
    Lowest applicable cruise cap for this curve.

    Two curvature-based caps (lowest wins):
      centripetal : v <= sqrt(a_max / kappa)          — XY radial acceleration
      A-axis slew : v <= a_rate / kappa               — tangential tool rotation

    A tangential tool rotates at dθ/dt = kappa·v (curvature × tool speed). Capping
    v so this stays within the A axis's slew rate (a_rate_deg_s, converted to
    rad/s) keeps the planned XY speed in step with what the A axis can actually
    follow — without it, stage6's per-axis interval limiter silently slows
    A-bound segments below their planned speed, creating velocity jumps. a_rate=0
    (non-tangential / unset) skips this cap.
    """
    cap = feed_max
    if m.kappa_max > 1e-9:
        cap = min(cap, math.sqrt(a_max / m.kappa_max))
        if a_rate_deg_s > 0.0:
            cap = min(cap, math.radians(a_rate_deg_s) / m.kappa_max)
    return cap

def _unit(v):
    l = math.hypot(v[0], v[1])
    return (v[0]/l, v[1]/l) if l > 1e-12 else (0.0, 0.0)

def _turn_angle_deg(curve_a, curve_b):
    """
    Turn angle at the joint between two curves, in degrees: 0 = straight,
    180 = full reversal. Direction-of-travel tangents (exit of A, entry of B).
    Returns 0.0 for a degenerate tangent (don't treat as a corner).
    """
    d_prev = _unit((curve_a.p3[0] - curve_a.p2[0], curve_a.p3[1] - curve_a.p2[1]))
    d_next = _unit((curve_b.p1[0] - curve_b.p0[0], curve_b.p1[1] - curve_b.p0[1]))
    if d_prev == (0.0, 0.0) or d_next == (0.0, 0.0):
        return 0.0
    dot = max(-1.0, min(1.0, d_prev[0]*d_next[0] + d_prev[1]*d_next[1]))
    return math.degrees(math.acos(dot))

def _junction_velocity(curve_a, curve_b, a_max, deviation, feed_max):
    """
    GRBL-style junction-deviation cornering speed at the joint between two
    curves. Models the corner as a circular arc deviating from the exact corner
    by at most `deviation` mm; returns the speed that keeps centripetal accel at
    a_max on that arc.

      straight join  -> feed_max (no slowdown)
      90-degree turn -> sqrt(a_max * 2.41 * deviation)
      full reversal  -> 0

    Tangent directions are direction-of-travel: exit of A, entry of B.
    """
    d_prev = _unit((curve_a.p3[0] - curve_a.p2[0], curve_a.p3[1] - curve_a.p2[1]))
    d_next = _unit((curve_b.p1[0] - curve_b.p0[0], curve_b.p1[1] - curve_b.p0[1]))
    if d_prev == (0.0, 0.0) or d_next == (0.0, 0.0):
        return feed_max  # degenerate tangent — don't constrain

    dot = max(-1.0, min(1.0, d_prev[0]*d_next[0] + d_prev[1]*d_next[1]))
    # half_cos = cos(turn_angle/2): 1 when straight, 0 at full reversal
    half_cos = math.sqrt(0.5 * (1.0 + dot))
    if half_cos >= 1.0 - 1e-9:
        return feed_max  # effectively straight
    radius = deviation * half_cos / (1.0 - half_cos)
    return min(feed_max, math.sqrt(a_max * radius))

def _triangular_peak(v_entry, v_exit, a_max, length):
    """
    Peak velocity for a triangular profile (when cruise can't be reached).
    Derived from: d_accel + d_decel = length
      (v_peak² - v_entry²)/(2a) + (v_peak² - v_exit²)/(2a) = length
    """
    return math.sqrt(max(0.0, (v_entry**2 + v_exit**2) / 2 + a_max * length))

# ── merge groups ──────────────────────────────────────────────────────────────

def _build_merge_groups(metrics, flags):
    """
    Returns list of groups, each a list of indices belonging to that group.
    MERGE_WITH_PREV attaches a curve to the previous group.
    """
    groups = []
    for i in range(len(metrics)):
        if i > 0 and (flags[i] & MERGE_WITH_PREV) and groups:
            groups[-1].append(i)
        else:
            groups.append([i])
    return groups

# ── main planner ──────────────────────────────────────────────────────────────

def plan_velocities(metrics, flags, feed_max, a_max, junction_deviation=None,
                    corner_stop_angle_deg=None, a_rate_deg_s=0.0):
    """
    Returns list of PlannedCurve.

    junction_deviation enables GRBL-style cornering: at each internal cusp
    (sharp join stage3 left unblended) the joint velocity is capped by the
    corner's turn angle, so the machine decelerates into sharp corners and
    accelerates out instead of charging through at full speed. Defaults to
    config.default().motion.junction_deviation.

    corner_stop_angle_deg, when set, forces v=0 at any internal junction whose
    turn angle meets or exceeds it. This pairs with the tangential tool's
    lift-pivot-lower (stage6): the blade can only pivot cleanly at a corner if
    XY has truly stopped first, so the planner must decelerate to 0 there — a
    stronger constraint than junction-deviation's low-but-nonzero corner speed.
    None (default) keeps the old behaviour (no forced stops).

    a_rate_deg_s is the tangential tool's A-axis slew ceiling (deg/s). When > 0
    it adds a per-curve curvature cap (v <= a_rate/kappa) so planned XY speed
    stays within what the A axis can follow on tight curves — eliminating the
    velocity jumps that arise when stage6's interval limiter slows A-bound
    segments below their planned speed. 0 (default) disables it (e.g. a pen).
    """
    n = len(metrics)
    if n == 0:
        return []

    if junction_deviation is None:
        junction_deviation = _config_default().motion.junction_deviation

    # Per-curve cap on v_exit[i] from the junction to curve i+1. inf where there
    # is no continuous junction (last curve, end of a path, or a path break).
    corner_cap = [float("inf")] * n
    for i in range(n - 1):
        if (flags[i] & PATH_END) or (flags[i + 1] & PATH_START):
            continue  # path boundary — a jog separates these, not a corner
        # A sharp corner where the tool will lift-pivot needs a full stop.
        if (corner_stop_angle_deg is not None and
                _turn_angle_deg(metrics[i].curve, metrics[i + 1].curve)
                >= corner_stop_angle_deg):
            corner_cap[i] = 0.0
            continue
        corner_cap[i] = _junction_velocity(
            metrics[i].curve, metrics[i + 1].curve, a_max, junction_deviation, feed_max)

    groups = _build_merge_groups(metrics, flags)

    # Working arrays — one entry per curve
    v_entry = [0.0] * n
    v_exit  = [0.0] * n
    v_cruise= [0.0] * n
    merged  = [False] * n

    # Mark merged curves
    for g in groups:
        if len(g) > 1:
            for idx in g:
                merged[idx] = True

    # ── forward pass ──────────────────────────────────────────────────────────
    # Operate on groups as atomic units

    group_v_entry = [0.0] * len(groups)  # entry velocity of each group

    for gi, g in enumerate(groups):
        # group entry velocity
        first_flags = flags[g[0]]
        if first_flags & PATH_START:
            gv_entry = 0.0
        elif gi == 0:
            gv_entry = 0.0
        else:
            gv_entry = group_v_entry[gi]  # set by previous group's exit

        group_length = sum(metrics[i].path_length_mm for i in g)
        group_cap    = min(_v_cruise_cap(metrics[i], feed_max, a_max, a_rate_deg_s) for i in g)

        gv_reachable = _v_reachable(gv_entry, a_max, group_length)
        gv_exit      = min(group_cap, gv_reachable, corner_cap[g[-1]])

        # propagate to next group
        if gi + 1 < len(groups):
            next_first_flags = flags[groups[gi+1][0]]
            if next_first_flags & PATH_START:
                group_v_entry[gi+1] = 0.0
            else:
                group_v_entry[gi+1] = gv_exit

        # distribute entry/exit across curves in group
        v_cur = gv_entry
        for idx in g:
            v_entry[idx] = v_cur
            reachable = _v_reachable(v_cur, a_max, metrics[idx].path_length_mm)
            cap = _v_cruise_cap(metrics[idx], feed_max, a_max, a_rate_deg_s)
            v_exit[idx] = min(cap, reachable, corner_cap[idx])
            v_cur = v_exit[idx]

    # ── backward pass ─────────────────────────────────────────────────────────

    for gi in range(len(groups) - 1, -1, -1):
        g = groups[gi]
        last_flags = flags[g[-1]]

        # force exit to 0 at PATH_END
        if last_flags & PATH_END:
            v_exit[g[-1]] = 0.0

        # walk backward within group
        for j in range(len(g) - 1, -1, -1):
            idx = g[j]
            v_ex = v_exit[idx]
            # what entry is needed to decelerate to v_exit within this length?
            v_limited = _v_reachable(v_ex, a_max, metrics[idx].path_length_mm)
            v_entry[idx] = min(v_entry[idx], v_limited)
            if j > 0:
                prev = g[j-1]
                v_exit[prev] = min(v_exit[prev], v_entry[idx])

        # propagate back to previous group's exit
        if gi > 0:
            prev_g = groups[gi - 1]
            v_exit[prev_g[-1]] = min(v_exit[prev_g[-1]], v_entry[g[0]])

    # ── compute v_cruise and build output ─────────────────────────────────────

    result = []
    for gi, g in enumerate(groups):
        group_length = sum(metrics[i].path_length_mm for i in g)
        group_cap    = min(_v_cruise_cap(metrics[i], feed_max, a_max, a_rate_deg_s) for i in g)
        gv_entry     = v_entry[g[0]]
        gv_exit      = v_exit[g[-1]]

        # triangular check: can we reach group_cap?
        d_accel = (group_cap**2 - gv_entry**2) / (2 * a_max)
        d_decel = (group_cap**2 - gv_exit**2)  / (2 * a_max)
        if d_accel + d_decel > group_length:
            group_cruise = _triangular_peak(gv_entry, gv_exit, a_max, group_length)
        else:
            group_cruise = group_cap

        for idx in g:
            v_cruise[idx] = group_cruise

    for i in range(n):
        result.append(PlannedCurve(
            metrics=metrics[i],
            flags=flags[i],
            v_entry=v_entry[i],
            v_cruise=v_cruise[i],
            v_exit=v_exit[i],
            merged=merged[i],
        ))

    return result

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    cfg = _config_default()
    parser = argparse.ArgumentParser(description="Stage 5: velocity planner")
    parser.add_argument("svg",       help="Path to SVG file")
    parser.add_argument("--feed-max",type=float, default=cfg.motion.feed_max, help="Max feed mm/s")
    parser.add_argument("--a-max",   type=float, default=cfg.motion.a_max,    help="Acceleration mm/s^2")
    parser.add_argument("--angle-tol",type=float,default=cfg.quality.angle_tol)
    parser.add_argument("--gap-tol",  type=float,default=cfg.quality.gap_tol)
    args = parser.parse_args()

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, _  = enforce_c1(curves_mm, args.angle_tol, args.gap_tol)
    metrics      = compute_metrics(repaired)

    # default: first curve PATH_START, last curve PATH_END, all others 0
    flags = [0] * len(metrics)
    if flags:
        flags[0]  |= PATH_START
        flags[-1] |= PATH_END

    planned = plan_velocities(metrics, flags, args.feed_max, args.a_max)

    total_len = sum(p.metrics.path_length_mm for p in planned)
    print(f"Curves: {len(planned)}   Total: {total_len:.2f} mm   "
          f"feed_max={args.feed_max}  a_max={args.a_max}\n")
    print(f"  {'#':>3}  {'length':>8}  {'v_entry':>8}  {'v_cruise':>8}  {'v_exit':>8}  {'merged'}")
    print(f"  {'-'*3}  {'-'*8}  {'-'*8}  {'-'*8}  {'-'*8}  {'------'}")
    for i, p in enumerate(planned):
        print(f"  {i:3d}  {p.metrics.path_length_mm:8.2f}  "
              f"{p.v_entry:8.2f}  {p.v_cruise:8.2f}  {p.v_exit:8.2f}  {p.merged}")
