"""Tests for the Discretize stage (redesign stages 7+8): Sample stream -> MicroSegments.

The decisive check is XY conservation: the emitted net step deltas move the tool
exactly from the path's first sample to its last (the accumulator telescopes to
round(last) - round(first)), with per-axis invert applied. Geometry in, correct
net displacement out, regardless of segment density.
"""

import sys, os, math
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))

from pipeline.stages.flatten import flatten
from pipeline.stages.constrain import constrain
from pipeline.stages.plan_lookahead import plan
from pipeline.stages.discretize import discretize
from pipeline.stages.microsegment import MICRO_PATH_END, MICRO_JOG, MICRO_LIFT
from pipeline.stages.stage2 import load_svg_mm_subpaths
from pipeline.stages.stage3 import enforce_c1
from pipeline.stages.config import default, KNIFE, PEN
from pipeline.stages.stage1 import CubicBezier
from pipeline.data.mock_curves import CASES

CFG = default()
MACH = CFG.machine
FEED = 80.0
A_MAX = 1000.0

def _line(p0, p1):
    d = ((p1[0]-p0[0])/3, (p1[1]-p0[1])/3)
    return CubicBezier(p0, (p0[0]+d[0], p0[1]+d[1]), (p1[0]-d[0], p1[1]-d[1]), p1)

def _new(subpaths, profile):
    a_rate = MACH.a.max_rate if profile.tangential else 0.0
    corner = profile.corner_angle_deg if profile.tangential else None
    s = flatten(subpaths, quality=CFG.quality)
    constrain(s, FEED, A_MAX, a_rate_deg_s=a_rate, corner_stop_angle_deg=corner)
    plan(s, MACH, a_max=A_MAX)
    return discretize(s, MACH, profile=profile, quality=CFG.quality)

def _net(segs):
    return (sum(s.dx for s in segs), sum(s.dy for s in segs), sum(s.da for s in segs))

def _expected_xy(subpaths):
    """Net emitted XY steps the tool must travel from first to last sample."""
    s = flatten(subpaths, quality=CFG.quality)
    x_spu, y_spu = MACH.x.steps_per_unit, MACH.y.steps_per_unit
    dx = round(s[-1].x * x_spu) - round(s[0].x * x_spu)
    dy = round(s[-1].y * y_spu) - round(s[0].y * y_spu)
    if MACH.x.invert: dx = -dx
    if MACH.y.invert: dy = -dy
    return dx, dy

# ── XY conservation: net steps land the tool at the geometric endpoint ────────

def test_net_xy_matches_geometry_snake():
    subpaths_mm, _ = load_svg_mm_subpaths(os.path.join(
        os.path.dirname(__file__), "..", "data", "test_snake.svg"))
    repaired = [enforce_c1(sp)[0] for sp in subpaths_mm]
    nx, ny, _ = _net(_new(repaired, KNIFE))
    assert (nx, ny) == _expected_xy(repaired)

def test_net_xy_matches_geometry_cases():
    for name, (curves, _) in CASES.items():
        nx, ny, _ = _net(_new([curves], KNIFE))
        assert (nx, ny) == _expected_xy([curves]), f"{name}: {(nx,ny)}"

# ── pen tool: no A rotation, no lift unless asked ─────────────────────────────

def test_pen_no_rotation():
    segs = _new([CASES["s_curve"][0]], PEN)
    assert all(s.da == 0 for s in segs)
    assert all(not (s.flags & MICRO_LIFT) for s in segs)

# ── path end flag ─────────────────────────────────────────────────────────────

def test_last_segment_path_end():
    segs = _new([CASES["straight_line"][0]], KNIFE)
    assert segs[-1].flags & MICRO_PATH_END

# ── corners produce a pivot ───────────────────────────────────────────────────

def test_corner_emits_pivot():
    horiz = _line((0.0, 0.0), (20.0, 0.0))
    vert  = _line((20.0, 0.0), (20.0, 20.0))
    segs = _new([[horiz, vert]], KNIFE)
    # a lift-pivot at the 90-degree corner -> pure-A MICRO_JOG segments present
    pivots = [s for s in segs if (s.flags & MICRO_JOG) and s.da != 0 and s.dx == 0 and s.dy == 0]
    assert pivots, "no pivot rotation emitted at the sharp corner"

# ── unwind keeps physical A bounded ───────────────────────────────────────────

def test_unwind_bounds_physical_a():
    # many small closed loops would wind A without unwind; KNIFE unwinds pen-up.
    # A single full circle: net A ~ +/-360 deg of tracking; physical should stay
    # within ~one turn since each PATH_START unwinds to the entry tangent.
    circle = CASES["full_circle_r30"][0]
    # build several separate subpaths of the same circle
    segs = _new([circle, circle, circle], KNIFE)
    # accumulate physical A (un-invert the emitted sign)
    a_inv = -1 if MACH.a.invert else 1
    phys = 0
    peak = 0
    for s in segs:
        phys += s.da * a_inv
        peak = max(peak, abs(phys))
    a_spd = MACH.a.steps_per_unit
    assert peak < 540 * a_spd, f"A wound to {peak/a_spd:.0f} deg (unwind failed)"

if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for t in tests:
        try:
            t(); print(f"  PASS  {t.__name__}"); passed += 1
        except Exception as e:
            print(f"  FAIL  {t.__name__}: {e}"); failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(failed)
