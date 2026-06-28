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
from flatten import flatten
from constrain import constrain
from plan_lookahead import plan
from discretize import discretize
from microsegment import MICRO_JOG, MICRO_LIFT, MICRO_PATH_END
from sample import PATH_START, PATH_END
from config import default as config_default, MachineConfig, KNIFE, PEN

JOG_FEED = config_default().machine.jog_feed


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

def build(svg_path, machine, feed_max, a_max, angle_tol, gap_tol, jog_feed=JOG_FEED,
          tangential=True):
    """Return (samples, microsegments, subpaths_repaired) via the per-sample pipeline."""
    subpaths_mm, _ = load_svg_mm_subpaths(svg_path)
    repaired = [enforce_c1(sp, angle_tol, gap_tol)[0] for sp in subpaths_mm]

    # Validate against the real tool profile (KNIFE/PEN) so corner stop, A-rate
    # cap and the A unwind all match what the host actually emits.
    profile = KNIFE if tangential else PEN
    corner_stop = profile.corner_angle_deg if tangential else None
    a_rate  = machine.a.max_rate if tangential else 0.0
    a_accel = machine.a.accel    if tangential else 0.0

    samples = flatten(repaired, quality=config_default().quality)
    constrain(samples, feed_max, a_max, a_rate_deg_s=a_rate,
              a_accel_deg_s2=a_accel, corner_stop_angle_deg=corner_stop)
    plan(samples, machine, a_max=a_max)
    segments = discretize(samples, machine, profile=profile, jog_feed=jog_feed)
    return samples, segments, repaired


def is_jog(seg):
    # "travel" — any non-drawing move (XY jog or Z pen lift). Excluded from the
    # XY drawing checks (geometry, conservation, velocity ceiling, boundaries).
    return bool(seg.flags & (MICRO_JOG | MICRO_LIFT))


# ── emitted-delta -> true geometry (mm) ──────────────────────────────────────────
# Emitted deltas are in physical step space: per-axis resolution and sign flipped
# by axis.invert. Undo both to recover the intended SVG geometry for the checks.

def _x_mm(seg, machine):
    s = -seg.dx if machine.x.invert else seg.dx
    return s / machine.x.steps_per_unit

def _y_mm(seg, machine):
    s = -seg.dy if machine.y.invert else seg.dy
    return s / machine.y.steps_per_unit


# ── per-segment kinematics ──────────────────────────────────────────────────────

def seg_kinematics(seg, machine):
    """Return (major_steps, time_s, xy_speed_mm_s) for one MicroSegment."""
    major = max(abs(seg.dx), abs(seg.dy), abs(seg.dz), abs(seg.da))
    if major == 0:
        return 0, 0.0, 0.0
    time_s = major * seg.interval / machine.f_cpu
    dist = math.hypot(_x_mm(seg, machine), _y_mm(seg, machine))
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


def check_acceleration(segments, machine, a_max, slack=1.5, window_mm=1.0):
    """
    Acceleration estimated over a distance window via a = |v_i^2-v_j^2|/(2d),
    NOT per-segment dv/dt. Discretize's velocity-aware subdivision produces uneven
    segment sizes — a lone 1-step segment between larger ones has a tiny dt that
    makes a per-pair dv/dt explode even when the continuous profile respects the
    accel limit. The distance-window kinematic estimate is robust to that
    quantization.

    window_mm is 1.0 (not a fraction of a mm): the per-segment velocity is
    RECONSTRUCTED from quantized step counts and the interval, so on short
    segments (a few steps) it carries quantization noise — adjacent near-identical
    segments can read v = 55, 71, 55 mm/s purely because the major axis flips
    between 1 and 2 steps. A sub-mm window divides the difference of two noisy
    reconstructions by a tiny distance and reports a spurious spike (6 kmm/s^2 on
    a path actually cruising at 80). Averaging over ~1 mm washes the quantization
    noise out and converges on the true planned accel — a window sweep shows the
    estimate decaying toward the planar bound as it widens.

    Ceiling is the true PLANAR accel bound hypot(x.accel, y.accel), not the scalar
    a_max: per-axis accel projection lets the tool accelerate up to that magnitude
    on a diagonal while each axis stays within its own limit (a pure 45° move at
    x.accel == y.accel reaches sqrt(2)*a_max). Putting the diagonal in the BOUND
    instead of a fudge slack keeps the check honest — it would still catch a
    genuine over-acceleration. The residual `slack` covers only discretization:
    discretize interpolates v linearly within a sample pair, so a window straddling
    a corner's v=0 dip reads a slightly high a. The Plan stage itself is
    acceleration-continuous at the samples.

    a_max is retained only for the report line / as a fallback when the machine
    carries no per-axis accel (uniform machine with accel == 0).
    """
    c = Check("acceleration continuity")
    planar = math.hypot(machine.x.accel, machine.y.accel)
    ceiling = planar if planar > 0 else a_max
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
            dist += math.hypot(_x_mm(segments[j], machine), _y_mm(segments[j], machine))
        if dist < 1e-6:
            continue
        a = abs(vs[i]**2 - vs[j]**2) / (2 * dist)
        worst = max(worst, a)
        if a > ceiling * slack:
            c.fail(f"a = {a:.0f} > planar accel {ceiling:.0f} mm/s^2 (x{slack} slack)")
    if c.passed:
        c.ok(f"peak {worst:.0f} mm/s^2 <= {ceiling:.0f}x{slack}")
    return c


def check_interval_bounds(segments, machine, feed_max, jog_feed, eps=0.02):
    """
    Verify no axis exceeds its per-axis step-rate ceiling (max_rate * spu) and no
    interval overflows. The old XY-only floor misread legitimate fast-A segments
    (A as major axis steps faster than XY would) as violations; this checks each
    axis's actual rate instead. Axes with max_rate == 0 are unconstrained.
    """
    c = Check("axis rate / interval bounds")
    ceiling = machine.f_cpu  # 1 step/sec saturation cap
    axes = (("X", machine.x), ("Y", machine.y), ("Z", machine.z), ("A", machine.a))
    lo, hi = None, None
    for s in segments:
        iv = s.interval
        lo = iv if lo is None else min(lo, iv)
        hi = iv if hi is None else max(hi, iv)
        if iv <= 0:
            c.fail(f"non-positive interval {iv}"); continue
        if iv > ceiling:
            c.fail(f"interval {iv} above ceiling {ceiling:.0f} (overflow risk)"); continue
        deltas = (s.dx, s.dy, s.dz, s.da)
        major = max(abs(d) for d in deltas)
        if major == 0:
            continue
        t_s = major * iv / machine.f_cpu
        for (name, ax), d in zip(axes, deltas):
            R = ax.max_rate * ax.steps_per_unit
            if R > 0 and d != 0:
                rate = abs(d) / t_s
                if rate > R * (1 + eps):
                    c.fail(f"{name} step rate {rate:.0f} > ceiling {R:.0f} steps/s")
    if c.passed:
        c.ok(f"interval range {lo}-{hi} cycles, all axis rates within ceilings")
    return c


def check_step_conservation(segments, samples, machine):
    c = Check("step conservation")
    spans = list(path_spans(segments))
    # Per-subpath geometry endpoints straight from the sample stream:
    # (first sample position, last sample position) between PATH_START/PATH_END.
    path_geom = []  # ((x0,y0), (x1,y1)) per path
    start = None
    for s in samples:
        if s.flags & PATH_START:
            start = (s.x, s.y)
        if s.flags & PATH_END and start is not None:
            path_geom.append((start, (s.x, s.y)))
            start = None

    if len(spans) != len(path_geom):
        c.fail(f"path count mismatch: {len(spans)} segment-paths vs {len(path_geom)} geometry-paths")
        return c

    xinv = -1 if machine.x.invert else 1
    yinv = -1 if machine.y.invert else 1
    for (s0, s1), (p_start, p_end) in zip(spans, path_geom):
        # Sum drawing segments only — jogs are inter-path travel, not geometry.
        # Un-invert to compare emitted steps against true geometry.
        net_x = xinv * sum(segments[i].dx for i in range(s0, s1 + 1) if not is_jog(segments[i]))
        net_y = yinv * sum(segments[i].dy for i in range(s0, s1 + 1) if not is_jog(segments[i]))
        exp_x = round((p_end[0] - p_start[0]) * machine.x.steps_per_unit)
        exp_y = round((p_end[1] - p_start[1]) * machine.y.steps_per_unit)
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


def check_geometry(segments, samples, machine, tol=0.1):
    """Reconstruct XY trajectory from steps; assert it tracks the flattened path.

    The flattened sample positions ARE the reference polyline — discretize
    quantizes exactly that, so the reconstructed trajectory must track it.
    """
    c = Check("geometric fidelity")

    # Reference polyline per path = the sample positions between PATH_START/END.
    ref = []  # list of paths, each a list of (x,y) sample points
    cur = None
    for s in samples:
        if s.flags & PATH_START:
            if cur is not None:
                ref.append(cur)
            cur = []
        cur.append((s.x, s.y))
    if cur is not None:
        ref.append(cur)

    spans = list(path_spans(segments))
    if len(spans) != len(ref):
        c.fail(f"path count mismatch ({len(spans)} vs {len(ref)})")
        return c

    worst = 0.0
    for (s0, s1), poly in zip(spans, ref):
        # Anchor at the path's true start and apply DRAWING segments only.
        # The leading jog repositioned the tool here; it isn't part of the path.
        # Deltas are mapped back to true geometry (per-axis + un-invert).
        x = poly[0][0]
        y = poly[0][1]
        recon = [(x, y)]
        for i in range(s0, s1 + 1):
            if is_jog(segments[i]):
                continue
            x += _x_mm(segments[i], machine)
            y += _y_mm(segments[i], machine)
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
    ap.add_argument("--steps-per-mm",  type=float, default=None,
                    help="Override XY steps/mm (default: real per-axis config)")
    ap.add_argument("--steps-per-deg", type=float, default=None,
                    help="Override A steps/deg (default: real per-axis config)")
    ap.add_argument("--f-cpu",         type=int,   default=cfg.machine.f_cpu)
    ap.add_argument("--feed-max",      type=float, default=KNIFE.feed_max)
    ap.add_argument("--a-max",         type=float, default=cfg.machine.x.accel)
    ap.add_argument("--angle-tol",     type=float, default=cfg.quality.angle_tol)
    ap.add_argument("--gap-tol",       type=float, default=cfg.quality.gap_tol)
    ap.add_argument("--geom-tol",      type=float, default=0.2,
                    help="Max allowed trajectory deviation, mm (default 0.2)")
    ap.add_argument("--tangential",    action=argparse.BooleanOptionalAction, default=True,
                    help="A-axis tangent tracking (knife/crease); --no-tangential for a pen")
    ap.add_argument("--jog-feed",      type=float, default=JOG_FEED,
                    help=f"Travel speed between subpaths, mm/s (default {JOG_FEED})")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    # Default to the real per-axis machine (honours Z/A resolution + invert);
    # scalar flags force a uniform machine only when explicitly given.
    if args.steps_per_mm is not None or args.steps_per_deg is not None:
        spm = args.steps_per_mm if args.steps_per_mm is not None else cfg.machine.x.steps_per_unit
        spd = args.steps_per_deg if args.steps_per_deg is not None else cfg.machine.a.steps_per_unit
        machine = MachineConfig.uniform(spm, spd, args.f_cpu)
    else:
        machine = cfg.machine

    samples, segments, _ = build(args.svg, machine, args.feed_max, args.a_max,
                                 args.angle_tol, args.gap_tol, jog_feed=args.jog_feed,
                                 tangential=args.tangential)

    print(f"SVG        : {args.svg}")
    print(f"Samples    : {len(samples)}")
    print(f"Segments   : {len(segments)}")
    print(f"Config     : X={machine.x.steps_per_unit} Y={machine.y.steps_per_unit} "
          f"A={machine.a.steps_per_unit} steps/unit, x.invert={machine.x.invert}, "
          f"feed_max {args.feed_max}, a_max {args.a_max}\n")

    if not segments:
        print("No segments generated — nothing to validate.")
        sys.exit(1)

    checks = [
        check_velocity_ceiling(segments, machine, args.feed_max, args.jog_feed),
        check_acceleration(segments, machine, args.a_max),
        check_interval_bounds(segments, machine, args.feed_max, args.jog_feed),
        check_step_conservation(segments, samples, machine),
        check_boundaries(segments, machine),
        check_geometry(segments, samples, machine, tol=args.geom_tol),
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
