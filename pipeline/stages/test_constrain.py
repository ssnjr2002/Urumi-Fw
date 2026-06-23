"""Tests for the Constrain stage (redesign stage 5): per-sample velocity ceiling."""

import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from flatten import flatten
from constrain import constrain, _junction_cap
from sample import Sample, CURVE_BOUNDARY
from stage1 import CubicBezier
from mock_curves import CASES

FEED = 80.0
A_MAX = 1000.0

def _line(p0, p1):
    d = ((p1[0]-p0[0])/3, (p1[1]-p0[1])/3)
    return CubicBezier(p0, (p0[0]+d[0], p0[1]+d[1]), (p1[0]-d[0], p1[1]-d[1]), p1)

# ── straight -> feed_max everywhere ───────────────────────────────────────────

def test_straight_is_feed_max():
    s = flatten([CASES["straight_line"][0]])
    constrain(s, FEED, A_MAX)
    assert all(abs(x.v_ceiling - FEED) < 1e-6 for x in s)

# ── circle -> constant centripetal cap ────────────────────────────────────────

def test_circle_centripetal_cap():
    # r5 circle: kappa=0.2 -> v <= sqrt(a/kappa) = sqrt(1000/0.2) ~ 70.7 mm/s < feed.
    s = flatten([CASES["quarter_circle_r5"][0]])
    constrain(s, FEED, A_MAX)
    expected = math.sqrt(A_MAX / 0.2)
    caps = [x.v_ceiling for x in s]
    assert all(c <= FEED + 1e-6 for c in caps)
    # the bulk should sit near the centripetal cap (constant-curvature arc)
    mid = caps[len(caps)//2]
    assert abs(mid - expected) / expected < 0.05

def test_tighter_circle_lower_cap():
    s50 = flatten([CASES["quarter_circle_r50"][0]]); constrain(s50, FEED, A_MAX)
    s5  = flatten([CASES["quarter_circle_r5"][0]]);  constrain(s5,  FEED, A_MAX)
    mid50 = s50[len(s50)//2].v_ceiling
    mid5  = s5[len(s5)//2].v_ceiling
    assert mid5 < mid50  # tighter -> slower

# ── A-slew cap ────────────────────────────────────────────────────────────────

def test_a_rate_cap_lowers_ceiling():
    # With a slow A axis the tangential cap a_rate/kappa should bite below the
    # centripetal cap on a tight curve.
    s_no = flatten([CASES["quarter_circle_r5"][0]]); constrain(s_no, FEED, A_MAX, a_rate_deg_s=0.0)
    s_a  = flatten([CASES["quarter_circle_r5"][0]]); constrain(s_a,  FEED, A_MAX, a_rate_deg_s=100.0)
    mid_no = s_no[len(s_no)//2].v_ceiling
    mid_a  = s_a[len(s_a)//2].v_ceiling
    assert mid_a < mid_no
    # value: rad(100)/0.2
    assert abs(mid_a - math.radians(100.0)/0.2) / mid_a < 0.05

# ── corner stop ───────────────────────────────────────────────────────────────

def test_sharp_corner_forces_zero():
    horiz = _line((0.0, 0.0), (10.0, 0.0))
    vert  = _line((10.0, 0.0), (10.0, 10.0))
    s = flatten([[horiz, vert]])
    constrain(s, FEED, A_MAX, corner_stop_angle_deg=20.0)
    bi = next(i for i, x in enumerate(s) if x.flags & CURVE_BOUNDARY)
    assert s[bi].v_ceiling == 0.0

def test_no_corner_stop_when_disabled():
    horiz = _line((0.0, 0.0), (10.0, 0.0))
    vert  = _line((10.0, 0.0), (10.0, 10.0))
    s = flatten([[horiz, vert]])
    constrain(s, FEED, A_MAX, corner_stop_angle_deg=None)
    bi = next(i for i, x in enumerate(s) if x.flags & CURVE_BOUNDARY)
    # no forced stop, but junction-deviation still caps it well below feed
    assert s[bi].v_ceiling > 0.0
    assert s[bi].v_ceiling < FEED

# ── junction cap helper ───────────────────────────────────────────────────────

def test_junction_cap_monotone():
    # sharper turn -> lower cap
    straight = _junction_cap(1.0,  A_MAX, 0.05, FEED)
    gentle   = _junction_cap(30.0, A_MAX, 0.05, FEED)
    sharp    = _junction_cap(120.0,A_MAX, 0.05, FEED)
    assert straight >= gentle >= sharp
    assert _junction_cap(0.0, A_MAX, 0.05, FEED) == FEED

# ── ceiling never exceeds feed ────────────────────────────────────────────────

def test_ceiling_bounded_by_feed():
    for name, (curves, _) in CASES.items():
        s = flatten([curves])
        constrain(s, FEED, A_MAX, a_rate_deg_s=100.0, corner_stop_angle_deg=20.0)
        assert all(x.v_ceiling <= FEED + 1e-6 for x in s), name

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
