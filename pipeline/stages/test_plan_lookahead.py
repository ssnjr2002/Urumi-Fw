"""Tests for the Plan stage (redesign stage 6): look-ahead feedrate planner.

The headline property is acceleration continuity: between every adjacent pair the
speed change must be feasible at the segment accel, in BOTH directions. The
tile-era planner could not guarantee this at curve junctions; this one does by
construction.
"""

import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from flatten import flatten
from constrain import constrain
from plan_lookahead import plan, _seg_accel, _subpath_ranges
from sample import PATH_START, PATH_END
from config import default
from stage1 import CubicBezier
from mock_curves import CASES

CFG = default()
MACH = CFG.machine
FEED = 80.0
A_MAX = 1000.0

def _line(p0, p1):
    d = ((p1[0]-p0[0])/3, (p1[1]-p0[1])/3)
    return CubicBezier(p0, (p0[0]+d[0], p0[1]+d[1]), (p1[0]-d[0], p1[1]-d[1]), p1)

def _prep(subpaths, a_rate=0.0, corner=None):
    s = flatten(subpaths, quality=CFG.quality)
    constrain(s, FEED, A_MAX, a_rate_deg_s=a_rate, corner_stop_angle_deg=corner)
    plan(s, MACH, a_max=A_MAX)
    return s

def _assert_accel_continuous(s):
    for lo, hi in _subpath_ranges(s):
        for i in range(lo, hi):
            ds = s[i].ds
            a = _seg_accel(s[i], s[i+1], MACH, A_MAX)
            budget = 2.0 * a * ds + 1e-6
            # forward feasible and backward feasible
            assert s[i+1].v**2 <= s[i].v**2 + budget + 1e-6, f"accel jump at {i}"
            assert s[i].v**2 <= s[i+1].v**2 + budget + 1e-6, f"decel jump at {i}"

# ── boundary conditions ───────────────────────────────────────────────────────

def test_endpoints_zero():
    s = _prep([CASES["s_curve"][0]])
    assert s[0].v == 0.0
    assert s[-1].v == 0.0

def test_v_within_ceiling():
    s = _prep([CASES["full_circle_r30"][0]])
    assert all(x.v <= x.v_ceiling + 1e-9 for x in s)

# ── the headline: acceleration continuity ─────────────────────────────────────

def test_accel_continuous_all_cases():
    for name, (curves, _) in CASES.items():
        s = _prep([curves], a_rate=100.0, corner=20.0)
        _assert_accel_continuous(s)

def test_accel_continuous_multi_subpath():
    s = _prep([CASES["straight_line"][0], CASES["quarter_circle_r5"][0]])
    _assert_accel_continuous(s)

# ── shape of the profile ──────────────────────────────────────────────────────

def test_straight_line_ramps_up_and_down():
    # 100mm straight: should accelerate from 0, reach feed, decelerate to 0.
    s = _prep([CASES["straight_line"][0]])
    vs = [x.v for x in s]
    peak = max(vs)
    assert peak > 0.9 * FEED          # long enough to reach (near) cruise
    assert vs[0] == 0.0 and vs[-1] == 0.0
    imax = vs.index(peak)
    assert all(vs[i] <= vs[i+1] + 1e-6 for i in range(imax))      # rising
    assert all(vs[i] >= vs[i+1] - 1e-6 for i in range(imax, len(vs)-1))  # falling

def test_short_line_triangular_peak_below_feed():
    # 1mm line can't reach feed from rest at a_max -> triangular, peak << feed.
    s = _prep([CASES["short_curve"][0]])
    peak = max(x.v for x in s)
    # reachable peak ~ sqrt(a_max * length) over a 1mm line, both ends at 0
    assert peak < FEED
    assert peak <= math.sqrt(A_MAX * 1.0) + 1.0

# ── corner stop ───────────────────────────────────────────────────────────────

def test_corner_brings_both_sides_to_zero():
    horiz = _line((0.0, 0.0), (20.0, 0.0))
    vert  = _line((20.0, 0.0), (20.0, 20.0))
    s = _prep([[horiz, vert]], a_rate=100.0, corner=20.0)
    from sample import CURVE_BOUNDARY
    bi = next(i for i, x in enumerate(s) if x.flags & CURVE_BOUNDARY)
    assert s[bi].v == 0.0
    assert s[bi-1].v < 1.0   # coincident prior sample also ~0 (zero-gap decel)

# ── per-axis accel ────────────────────────────────────────────────────────────

def test_diagonal_accel_exceeds_scalar():
    # On a 45-degree line, per-axis projection lets the tool accelerate faster
    # than the scalar a_max (each axis still within its own limit). Use a high
    # feed so the cap doesn't mask the accel headroom near the start.
    diag = _line((0.0, 0.0), (100.0, 100.0))
    s = flatten([[diag]], quality=CFG.quality)
    constrain(s, 300.0, A_MAX)          # feed well above the diagonal accel reach
    plan(s, MACH, a_max=A_MAX)
    acc = 0.0
    for i in range(len(s)-1):
        acc += s[i].ds
        if acc > 5.0:
            scalar_bound = math.sqrt(2.0 * A_MAX * acc)   # what a scalar a_max gives
            assert s[i].v > scalar_bound * 1.05, f"v={s[i].v} bound={scalar_bound}"
            break

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
