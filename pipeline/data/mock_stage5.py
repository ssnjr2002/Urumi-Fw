"""
Mock input data for stage 5 (velocity planner).
Each case provides:
  - metrics:  list of CurveMetrics (from stage 4)
  - flags:    parallel list of flag sets per curve (PATH_START, PATH_END, MERGE_WITH_PREV)
  - feed_max: float mm/s  — tool cruise limit
  - a_max:    float mm/s² — machine acceleration limit
  - expected: dict of assertions the test will check (None = no analytical value)
"""

import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "stages"))
from stage1 import CubicBezier
from stage4 import CurveMetrics, kappa_samples, arc_length

# ── flags ─────────────────────────────────────────────────────────────────────

PATH_START      = 0x01
PATH_END        = 0x02
MERGE_WITH_PREV = 0x04

# ── helpers ───────────────────────────────────────────────────────────────────

def pt(x, y):
    return (float(x), float(y))

def make_metrics(p0, p1, p2, p3):
    c = CubicBezier(pt(*p0), pt(*p1), pt(*p2), pt(*p3))
    samples = kappa_samples(c)
    return CurveMetrics(
        curve=c,
        path_length_mm=arc_length(c),
        kappa_max=max(k for _, k in samples),
        kappa_samples=samples,
    )

def straight(x0, x1, y=0):
    """Horizontal straight line from x0 to x1."""
    dx = (x1 - x0) / 3
    return make_metrics((x0, y), (x0+dx, y), (x1-dx, y), (x1, y))

def arc_quarter(r, ox=0, oy=0):
    """Quarter circle of radius r, centred at (ox, oy)."""
    h = r * 0.5522847
    return make_metrics((ox+r, oy), (ox+r, oy+h), (ox+h, oy+r), (ox, oy+r))

# ── case 1: single curve, start+end → must stop at both ends ─────────────────
# v_entry=0, v_exit=0. With 100mm length and a_max=500, v_peak = sqrt(a*L) = ~224
# but feed_max=150 so profile is trapezoidal if distance allows, else triangular.

SINGLE_START_END = dict(
    metrics=[straight(0, 100)],
    flags=[PATH_START | PATH_END],
    feed_max=150.0,
    a_max=500.0,
    expected={"v_entry_0": 0.0, "v_exit_0": 0.0},
)

# ── case 2: long straight, start+end → should reach feed_max cruise ───────────
# d_accel = v²/2a = 150²/1000 = 22.5mm. 500mm >> 2*22.5mm → flat top guaranteed.

LONG_STRAIGHT_CRUISE = dict(
    metrics=[straight(0, 500)],
    flags=[PATH_START | PATH_END],
    feed_max=150.0,
    a_max=1000.0,
    expected={"v_entry_0": 0.0, "v_exit_0": 0.0, "v_cruise_reaches_feed_max": True},
)

# ── case 3: short curve — triangular profile (can't reach feed_max) ───────────
# Length=10mm, feed_max=200, a_max=1000.
# d_required = 2*(200²/2000) = 40mm > 10mm → triangular, v_peak < 200.
# v_peak = sqrt(a_max * L) = sqrt(1000*10) = 100 mm/s

SHORT_TRIANGULAR = dict(
    metrics=[straight(0, 10)],
    flags=[PATH_START | PATH_END],
    feed_max=200.0,
    a_max=1000.0,
    expected={"v_entry_0": 0.0, "v_exit_0": 0.0, "v_cruise_lt_feed_max": True},
)

# ── case 4: chain of 3 curves — middle should cruise ─────────────────────────
# Each 100mm. Forward pass: [0] accel from 0, [1] cruise, [2] decel to 0.
# feed_max=100, a_max=500 → d_stop = 100²/1000 = 10mm << 100mm → flat top.

CHAIN_THREE = dict(
    metrics=[straight(0,100), straight(100,200), straight(200,300)],
    flags=[PATH_START, 0, PATH_END],
    feed_max=100.0,
    a_max=500.0,
    expected={"v_entry_0": 0.0, "v_exit_2": 0.0, "v_entry_1_gt_0": True},
)

# ── case 5: tight corner — centripetal limit forces speed reduction ───────────
# Straight 100mm → tight arc (r=5mm, kappa=0.2) → straight 100mm.
# v_centripetal = sqrt(a_max / kappa_max) = sqrt(500/0.2) = 50 mm/s.
# feed_max=200 → corner caps at 50. Backward pass must pull entry of arc down.

_R5 = 5.0
TIGHT_CORNER = dict(
    metrics=[
        straight(0, 100),
        arc_quarter(_R5),   # quarter arc r=5, kappa ≈ 1/5 = 0.2
        straight(0, 100),
    ],
    flags=[PATH_START, 0, PATH_END],
    feed_max=200.0,
    a_max=500.0,
    expected={"corner_v_capped": True, "v_cap_approx": math.sqrt(500/0.2)},
)

# ── case 6: two separate paths (two PATH_START/PATH_END pairs) ────────────────
# Each path starts and ends at zero. Velocities must not bleed across paths.

TWO_PATHS = dict(
    metrics=[straight(0,80), straight(0,80)],
    flags=[PATH_START | PATH_END, PATH_START | PATH_END],
    feed_max=100.0,
    a_max=500.0,
    expected={"v_entry_0": 0.0, "v_exit_0": 0.0, "v_entry_1": 0.0, "v_exit_1": 0.0},
)

# ── case 7: backward pass must tighten — decel distance insufficient ──────────
# Three curves: 200mm, 5mm, 200mm. feed_max=150, a_max=1000.
# d_stop = 150²/2000 = 11.25mm. Short middle segment (5mm) can't carry full speed.
# Backward pass must reduce v_exit of curve[0] to allow stopping within 5mm.

BACKWARD_TIGHTEN = dict(
    # d_stop from feed_max=150, a_max=1000: 150²/2000 = 11.25mm
    # curve[2] is only 8mm — can't decelerate to 0 from feed_max.
    # Backward pass must pull v_entry of curve[2], and that propagates
    # back to tighten v_exit of curve[0] and curve[1].
    # Max speed entering curve[2]: sqrt(2*1000*8) = 126.5 mm/s
    metrics=[straight(0,200), straight(200,205), straight(205,213)],
    flags=[PATH_START, 0, PATH_END],
    feed_max=150.0,
    a_max=1000.0,
    expected={"v_exit_0_lt_feed_max": True, "v_entry_2_approx": math.sqrt(2*1000*8)},
)

# ── case 8: merge flag — two short curves share one velocity envelope ─────────
# Both 8mm. Individually too short to cruise; merged total = 16mm.
# feed_max=200, a_max=1000. d_req = 2*(200²/2000)=40mm > 16mm → still triangular
# but higher v_peak than either alone.

MERGE_TWO_SHORT = dict(
    metrics=[straight(0,8), straight(8,16)],
    flags=[PATH_START, MERGE_WITH_PREV | PATH_END],
    feed_max=200.0,
    a_max=1000.0,
    expected={"v_entry_0": 0.0, "v_exit_1": 0.0, "merged": True},
)

# ── registry ──────────────────────────────────────────────────────────────────

CASES = {
    "single_start_end":      SINGLE_START_END,
    "long_straight_cruise":  LONG_STRAIGHT_CRUISE,
    "short_triangular":      SHORT_TRIANGULAR,
    "chain_three":           CHAIN_THREE,
    "tight_corner":          TIGHT_CORNER,
    "two_paths":             TWO_PATHS,
    "backward_tighten":      BACKWARD_TIGHTEN,
    "merge_two_short":       MERGE_TWO_SHORT,
}

if __name__ == "__main__":
    for name, case in CASES.items():
        metrics = case["metrics"]
        flags   = case["flags"]
        print(f"{name}  ({len(metrics)} curve(s))  feed_max={case['feed_max']}  a_max={case['a_max']}")
        for i, (m, f) in enumerate(zip(metrics, flags)):
            flag_str = "|".join(filter(None, [
                "PATH_START"      if f & PATH_START      else "",
                "PATH_END"        if f & PATH_END        else "",
                "MERGE_WITH_PREV" if f & MERGE_WITH_PREV else "",
            ])) or "none"
            print(f"  [{i}] length={m.path_length_mm:.2f}mm  kappa_max={m.kappa_max:.4f}  flags={flag_str}")
        print()
