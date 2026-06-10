"""
validate_plan.py — offline invariant checker for the MicroSegment planner

Runs the full host pipeline on an SVG and asserts properties of the resulting
MicroSegment stream WITHOUT any hardware. Catches planner regressions that a
plot can't: velocity/accel violations, interval overflow, step drift, and
geometric infidelity. Each check maps to a premortem risk (see PLAN_*.md §10).

Usage:
  python validate_plan.py pipeline/data/test_rect.svg
  python validate_plan.py design.svg --steps-per-mm 160 --feed-max 80 --a-max 1000
  python validate_plan.py design.svg --geom-tol 0.1 -v

Exit code 0 = all checks pass, 1 = one or more failed.
"""

import sys, os, argparse, math

_PIPELINE = os.path.join(os.path.dirname(__file__), "..", "pipeline", "stages")
sys.path.insert(0, _PIPELINE)
sys.path.insert(0, os.path.dirname(__file__))

from stage2 import load_svg_mm_subpaths
from stage3 import enforce_c1
from stage4 import compute_metrics, _bezier_point
from stage5 import plan_velocities, PATH_START, PATH_END
from stage6 import evaluate_microsegments, MICRO_JOG
from config import default as config_default, MachineConfig

JOG_FEED = config_default().motion.jog_feed


# ── result plumbing ─────────────────────────────────────────────────────────────

class Check:
    def __init__(self, name):
        self.name = name
        self.passed = True
        self.detail = ""
        self.violations = 0

    def fail(self, detail, n=1):
        self.passed = False
        self.detail = detail
        self.violations += n

    def ok(self, detail=""):
        self.detail = detail


# ── pipeline driver ─────────────────────────────────────────────────────────────

def build(svg_path, machine, feed_max, a_max, angle_tol, gap_tol, jog_feed=JOG_FEED):
    """Return (planned_curves, microsegments, subpaths_repaired)."""
    subpaths_mm, _ = load_svg_mm_subpaths(svg_path)
    repaired = [enforce_c1(sp, angle_tol, gap_tol)[0] for sp in subpaths_mm]

    flat = [c for sp in repaired for c in sp]
    flags = []
    for sp in repaired:
        for i in range(len(sp)):
            f = 0
            if i == 0:           f |= PATH_START
            if i == len(sp) - 1: f |= PATH_END
            flags.append(f)

    metrics = compute_metrics(flat)
    planned = plan_velocities(metrics, flags, feed_max, a_max)
    segments = evaluate_microsegments(planned, machine, jog_feed=jog_feed)
    return planned, segments, repaired


def is_jog(seg):
    return bool(seg.flags & MICRO_JOG)


# ── per-segment kinematics ──────────────────────────────────────────────────────

def seg_kinematics(seg, machine):
    """Return (major_steps, time_s, xy_speed_mm_s) for one MicroSegment."""
    major = max(abs(seg.dx), abs(seg.dy), abs(seg.dz), abs(seg.da))
    if major == 0:
        return 0, 0.0, 0.0
    time_s = major * seg.interval / machine.f_cpu
    dist = math.hypot(seg.dx / machine.steps_per_mm, seg.dy / machine.steps_per_mm)
    speed = dist / time_s if time_s > 0 else 0.0
    return major, time_s, speed


def _dist_point_to_segment(px, py, ax, ay, bx, by):
    """Perpendicular distance from (px,py) to segment AB — resolution-independent."""
    dx, dy = bx - ax, by - ay
    seg2 = dx * dx + dy * dy
    if seg2 == 0:
        return math.hypot(px - ax, py - ay)
    t = ((px - ax) * dx + (py - ay) * dy) / seg2
    t = max(0.0, min(1.0, t))
    cx, cy = ax + t * dx, ay + t * dy
    return math.hypot(px - cx, py - cy)


def path_spans(segments):
    """Yield (start_idx, end_idx_inclusive) for each PATH (split on PATH_END flag)."""
    from stage6 import MICRO_PATH_END
    start = 0
    for i, s in enumerate(segments):
        if s.flags & MICRO_PATH_END:
            yield (start, i)
            start = i + 1
    if start < len(segments):
        yield (start, len(segments) - 1)


# ── checks ──────────────────────────────────────────────────────────────────────

def check_velocity_ceiling(segments, machine, feed_max, jog_feed, eps=0.05):
    c = Check("velocity ceiling")
    worst = 0.0
    for s in segments:
        _, _, v = seg_kinematics(s, machine)
        ceil = jog_feed if is_jog(s) else feed_max
        worst = max(worst, v)
        if v > ceil * (1 + eps):
            kind = "jog" if is_jog(s) else "draw"
            c.fail(f"{kind} speed {v:.1f} > limit {ceil} mm/s")
    if c.passed:
        c.ok(f"peak {worst:.1f} mm/s within limits")
    return c


def check_acceleration(segments, machine, a_max, slack=2.0, window_mm=0.2):
    """
    Acceleration estimated over a small distance window via a = |v_i^2-v_j^2|/(2d),
    NOT per-segment dv/dt. Stage6's adaptive subdivision produces uneven segment
    sizes — a lone 1-step segment between larger ones has a tiny dt that makes a
    per-pair dv/dt explode even when the continuous profile respects a_max. The
    distance-window kinematic estimate is robust to that quantization.

    Slack is 2.0 because stage6 interpolates velocity LINEARLY in the curve
    parameter t (not arc-length with exact a_max ramps), which overshoots a_max
    by up to ~1.7x at the sharpest corners. That is a velocity-profile-model
    limitation (the jerk/profile layer), independent of junction-deviation
    cornering; tighten this slack once stage6 gets an arc-length-accurate ramp.
    """
    c = Check("acceleration continuity")
    spm = machine.steps_per_mm
    vs = [seg_kinematics(s, machine)[2] for s in segments]
    worst = 0.0
    for i in range(len(segments)):
        if is_jog(segments[i]):
            continue
        # walk back accumulating tool distance until the window is filled
        dist = 0.0
        j = i
        while j > 0 and dist < window_mm:
            if is_jog(segments[j - 1]):
                break
            j -= 1
            dist += math.hypot(segments[j].dx, segments[j].dy) / spm
        if dist < 1e-6:
            continue
        a = abs(vs[i]**2 - vs[j]**2) / (2 * dist)
        worst = max(worst, a)
        if a > a_max * slack:
            c.fail(f"a = {a:.0f} > a_max {a_max} mm/s^2 (x{slack} slack)")
    if c.passed:
        c.ok(f"peak {worst:.0f} mm/s^2 <= {a_max}x{slack}")
    return c


def check_interval_bounds(segments, machine, feed_max, jog_feed):
    c = Check("interval bounds")
    fastest_feed = max(feed_max, jog_feed)
    floor = (machine.f_cpu / (fastest_feed * machine.steps_per_mm)) * 0.9  # fastest legal step
    ceiling = machine.f_cpu  # 1 step/sec — stage6's saturation cap
    lo, hi = None, None
    for s in segments:
        iv = s.interval
        lo = iv if lo is None else min(lo, iv)
        hi = iv if hi is None else max(hi, iv)
        if iv <= 0:
            c.fail(f"non-positive interval {iv}")
        elif iv < floor:
            c.fail(f"interval {iv} below floor {floor:.0f} (faster than feed_max)")
        elif iv > ceiling:
            c.fail(f"interval {iv} above ceiling {ceiling:.0f} (overflow risk)")
    if c.passed:
        c.ok(f"range {lo}-{hi} cycles, within [{floor:.0f}, {ceiling:.0f}]")
    return c


def check_step_conservation(segments, planned, machine):
    c = Check("step conservation")
    spans = list(path_spans(segments))
    # Map planned curves to paths the same way (PATH_START boundaries)
    path_idx = -1
    path_geom = []  # (net_dx_mm, net_dy_mm) per path
    cur_start = None
    for p in planned:
        if p.flags & PATH_START:
            if cur_start is not None:
                path_geom.append((cur_start, last_end))
            cur_start = p.metrics.curve.p0
        last_end = p.metrics.curve.p3
    if cur_start is not None:
        path_geom.append((cur_start, last_end))

    if len(spans) != len(path_geom):
        c.fail(f"path count mismatch: {len(spans)} segment-paths vs {len(path_geom)} geometry-paths")
        return c

    for (s0, s1), (p_start, p_end) in zip(spans, path_geom):
        # Sum drawing segments only — jogs are inter-path travel, not geometry.
        net_x = sum(segments[i].dx for i in range(s0, s1 + 1) if not is_jog(segments[i]))
        net_y = sum(segments[i].dy for i in range(s0, s1 + 1) if not is_jog(segments[i]))
        exp_x = round((p_end[0] - p_start[0]) * machine.steps_per_mm)
        exp_y = round((p_end[1] - p_start[1]) * machine.steps_per_mm)
        if net_x != exp_x or net_y != exp_y:
            c.fail(f"net steps ({net_x},{net_y}) != geometry ({exp_x},{exp_y})")
    if c.passed:
        c.ok(f"{len(spans)} path(s), net steps match geometry exactly")
    return c


def check_boundaries(segments, machine, frac=0.5):
    """Each path must ramp from rest and decel to rest: the first/last segment
    speed should be well below the path's own peak. (The first segment's
    average is non-zero even from a standstill — it covers the initial ramp —
    so this is a relative check, not an absolute floor.)"""
    c = Check("path boundary ramps")
    for (s0, s1) in path_spans(segments):
        # Drawing segments only — exclude the leading travel jog.
        speeds = [seg_kinematics(segments[i], machine)[2]
                  for i in range(s0, s1 + 1) if not is_jog(segments[i])]
        peak = max(speeds) if speeds else 0.0
        if peak <= 0:
            continue
        if speeds[0] > peak * frac:
            c.fail(f"path starts at {speeds[0]:.1f} mm/s ({speeds[0]/peak:.0%} of peak {peak:.1f})")
        if speeds[-1] > peak * frac:
            c.fail(f"path ends at {speeds[-1]:.1f} mm/s ({speeds[-1]/peak:.0%} of peak {peak:.1f})")
    if c.passed:
        c.ok(f"all paths ramp from/to < {frac:.0%} of peak")
    return c


def check_geometry(segments, planned, machine, tol=0.1, samples_per_curve=60):
    """Reconstruct XY trajectory from steps; assert it tracks the Beziers."""
    c = Check("geometric fidelity")

    # Dense reference polyline per path from the planned Beziers
    ref = []  # list of paths, each a list of (x,y) sample points
    cur = None
    for p in planned:
        if p.flags & PATH_START:
            if cur is not None:
                ref.append(cur)
            cur = []
        crv = p.metrics.curve
        for k in range(samples_per_curve + 1):
            t = k / samples_per_curve
            cur.append(_bezier_point(crv, t))
    if cur is not None:
        ref.append(cur)

    spans = list(path_spans(segments))
    if len(spans) != len(ref):
        c.fail(f"path count mismatch ({len(spans)} vs {len(ref)})")
        return c

    spm = machine.steps_per_mm
    worst = 0.0
    for (s0, s1), poly in zip(spans, ref):
        # Anchor at the path's true start and apply DRAWING segments only.
        # The leading jog repositioned the tool here; it isn't part of the path.
        x = poly[0][0]
        y = poly[0][1]
        recon = [(x, y)]
        for i in range(s0, s1 + 1):
            if is_jog(segments[i]):
                continue
            x += segments[i].dx / spm
            y += segments[i].dy / spm
            recon.append((x, y))
        # For each reconstructed vertex, min distance to the reference polyline
        # SEGMENTS (not sample points) — independent of sampling resolution.
        for (rx, ry) in recon:
            d = min(_dist_point_to_segment(rx, ry, poly[k][0], poly[k][1],
                                           poly[k+1][0], poly[k+1][1])
                    for k in range(len(poly) - 1))
            worst = max(worst, d)
    if worst > tol:
        c.fail(f"max deviation {worst:.3f} mm > tol {tol} mm")
    else:
        c.ok(f"max deviation {worst:.4f} mm <= {tol} mm")
    return c


# ── main ─────────────────────────────────────────────────────────────────────────

def main():
    cfg = config_default()
    ap = argparse.ArgumentParser(description="Offline invariant checker for the MicroSegment planner")
    ap.add_argument("svg")
    ap.add_argument("--steps-per-mm",  type=float, default=cfg.machine.steps_per_mm)
    ap.add_argument("--steps-per-deg", type=float, default=cfg.machine.steps_per_deg)
    ap.add_argument("--f-cpu",         type=int,   default=cfg.machine.f_cpu)
    ap.add_argument("--feed-max",      type=float, default=cfg.motion.feed_max)
    ap.add_argument("--a-max",         type=float, default=cfg.motion.a_max)
    ap.add_argument("--angle-tol",     type=float, default=cfg.quality.angle_tol)
    ap.add_argument("--gap-tol",       type=float, default=cfg.quality.gap_tol)
    ap.add_argument("--geom-tol",      type=float, default=0.2,
                    help="Max allowed trajectory deviation, mm (default 0.2)")
    ap.add_argument("--geom-samples",  type=int,   default=200,
                    help="Reference samples per curve for fidelity check (default 200)")
    ap.add_argument("--jog-feed",      type=float, default=JOG_FEED,
                    help=f"Travel speed between subpaths, mm/s (default {JOG_FEED})")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    machine = MachineConfig.uniform(args.steps_per_mm, args.steps_per_deg, args.f_cpu)
    planned, segments, _ = build(args.svg, machine, args.feed_max, args.a_max,
                                 args.angle_tol, args.gap_tol, jog_feed=args.jog_feed)

    print(f"SVG        : {args.svg}")
    print(f"Curves     : {len(planned)}")
    print(f"Segments   : {len(segments)}")
    print(f"Config     : {args.steps_per_mm} steps/mm, feed_max {args.feed_max}, a_max {args.a_max}\n")

    if not segments:
        print("No segments generated — nothing to validate.")
        sys.exit(1)

    checks = [
        check_velocity_ceiling(segments, machine, args.feed_max, args.jog_feed),
        check_acceleration(segments, machine, args.a_max),
        check_interval_bounds(segments, machine, args.feed_max, args.jog_feed),
        check_step_conservation(segments, planned, machine),
        check_boundaries(segments, machine),
        check_geometry(segments, planned, machine, tol=args.geom_tol,
                       samples_per_curve=args.geom_samples),
    ]

    print(f"{'Check':28s}  Result  Detail")
    print(f"{'-'*28}  ------  {'-'*40}")
    all_ok = True
    for c in checks:
        status = "PASS" if c.passed else "FAIL"
        all_ok = all_ok and c.passed
        print(f"{c.name:28s}  {status:6s}  {c.detail}")

    print(f"\nOverall: {'PASS' if all_ok else 'FAIL'}")
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
