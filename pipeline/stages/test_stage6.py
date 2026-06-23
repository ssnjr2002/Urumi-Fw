"""Tests for stage 6: Bezier evaluation -> MicroSegments."""

import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from stage6 import (evaluate_microsegments, MicroSegment, MICRO_PATH_END,
                    MICRO_JOG, MICRO_LIFT)
from config import default as _cfg_default

V_MIN = _cfg_default().quality.v_min
from stage2 import load_svg_mm
from stage3 import enforce_c1
from stage4 import compute_metrics
from stage5 import plan_velocities, PATH_START, PATH_END
from mock_stage6 import CASES, MACHINE_DEFAULT

DATA = os.path.join(os.path.dirname(__file__), "..", "data")

def svg(name):
    return os.path.join(DATA, name)

def approx(a, b, tol):
    return abs(a - b) <= tol

def segs(name):
    case = CASES[name]
    return evaluate_microsegments(case["planned"], case["machine"])

# ── output structure ──────────────────────────────────────────────────────────

def test_returns_microsegments():
    s = segs("straight_constant_v")
    assert all(isinstance(x, MicroSegment) for x in s)

def test_nonempty_output():
    for name in CASES:
        s = segs(name)
        assert len(s) > 0, f"{name}: no segments produced"

def test_empty_planned_returns_empty():
    result = evaluate_microsegments([], MACHINE_DEFAULT)
    assert result == []

# ── straight line: dx only, constant interval ─────────────────────────────────

def test_straight_dy_zero():
    s = segs("straight_constant_v")
    assert all(x.dy == 0 for x in s)

def test_straight_da_zero():
    s = segs("straight_constant_v")
    assert all(x.da == 0 for x in s)

def test_straight_constant_interval():
    s = segs("straight_constant_v")
    ivs = [x.interval for x in s]
    assert max(ivs) == min(ivs)

def test_straight_interval_value():
    case = CASES["straight_constant_v"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    expected = case["machine"].f_cpu // (50 * case["machine"].steps_per_mm)
    assert approx(s[0].interval, expected, tol=500)

def test_straight_total_dx():
    case = CASES["straight_constant_v"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    total = sum(x.dx for x in s)
    # 100mm * 80 steps/mm = 8000 steps
    assert approx(total, 8000, tol=2)

# ── diagonal: dx ≈ dy ────────────────────────────────────────────────────────

def test_diagonal_dx_equals_dy():
    s = segs("diagonal_constant_v")
    total_dx = sum(x.dx for x in s)
    total_dy = sum(x.dy for x in s)
    assert approx(total_dx, total_dy, tol=2)

def test_diagonal_da_zero():
    # A straight diagonal needs no tangent rotation DURING the cut. (The blade
    # is oriented to the line by a pre-orientation jog before the cut — that
    # segment is flagged MICRO_JOG and excluded here.)
    s = segs("diagonal_constant_v")
    cut = [x for x in s if not (x.flags & (MICRO_JOG | MICRO_LIFT))]
    assert all(x.da == 0 for x in cut)

# ── acceleration: intervals decrease ─────────────────────────────────────────

def test_accel_total_steps():
    case = CASES["accel_straight"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    total = sum(x.dx for x in s)
    assert approx(total, 50 * 80, tol=5)

def test_accel_intervals_decrease():
    s = segs("accel_straight")
    ivs = [x.interval for x in s]
    # first interval should be larger than last (slow start, fast end)
    assert ivs[0] > ivs[-1]

def test_accel_first_interval_slow():
    # at v_entry→V_MIN, first interval ≈ f_cpu / (V_MIN * steps_per_mm)
    case = CASES["accel_straight"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    max_iv = case["machine"].f_cpu // int(V_MIN * case["machine"].steps_per_mm)
    assert s[0].interval <= max_iv

# ── deceleration: intervals increase ─────────────────────────────────────────

def test_decel_intervals_increase():
    s = segs("decel_to_stop")
    ivs = [x.interval for x in s]
    assert ivs[-1] > ivs[0]

def test_decel_last_interval_largest():
    s = segs("decel_to_stop")
    ivs = [x.interval for x in s]
    assert ivs[-1] == max(ivs)

# ── arc tangent tracking ──────────────────────────────────────────────────────

def test_arc_total_da():
    # Tangent tracking over the arc itself — exclude the pre-orientation jog
    # that rotates the blade to the arc's entry tangent before cutting.
    case = CASES["arc_tangent_tracking"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    total_da = sum(x.da for x in s if not (x.flags & (MICRO_JOG | MICRO_LIFT)))
    expected = case["expected"]["total_da_approx"]
    assert approx(abs(total_da), expected, tol=20)

def test_arc_net_displacement():
    case = CASES["arc_tangent_tracking"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    total_dx = abs(sum(x.dx for x in s))
    total_dy = abs(sum(x.dy for x in s))
    expected = case["expected"]["net_dx_approx"]
    assert approx(total_dx, expected, tol=5)
    assert approx(total_dy, expected, tol=5)

# ── A-axis pre-orientation (tangential tool) ─────────────────────────────────

def test_diagonal_preorient_emitted():
    # The blade must orient to the 45deg line before cutting. The pre-orientation
    # is a RAMPED pure-A move (multiple MICRO_JOG segments, no XY); their da sums
    # to ~45 deg.
    s = segs("diagonal_constant_v")
    preorient = [x for x in s if (x.flags & MICRO_JOG) and x.da != 0]
    assert len(preorient) >= 1
    assert all(x.dx == 0 and x.dy == 0 for x in preorient)   # pure rotation
    spd = MACHINE_DEFAULT.a.steps_per_unit
    total_da = sum(x.da for x in preorient)
    assert approx(abs(total_da), 45 * spd, tol=spd)  # within ~1 degree

def test_no_preorient_when_not_tangential():
    # With tangential off (pen), no A motion at all.
    case = CASES["diagonal_constant_v"]
    s = evaluate_microsegments(case["planned"], case["machine"], tangential=False)
    assert all(x.da == 0 for x in s)

# ── lift-pivot-lower corners (tangential tool) ───────────────────────────────

def _L_path():
    # Horizontal (exit tangent 0deg) into vertical (entry tangent 90deg): a 90deg
    # internal corner that must trigger lift-pivot-lower.
    from mock_stage6 import make_planned
    horiz = make_planned((0, 0), (3.3, 0), (6.7, 0), (10, 0),
                         0, 50, 50, flags=PATH_START)
    vert  = make_planned((10, 0), (10, 3.3), (10, 6.7), (10, 10),
                         50, 50, 0, flags=PATH_END)
    return [horiz, vert]

def test_corner_lift_pivot_lower():
    from config import KNIFE
    from stage6 import build_toolpath
    s = build_toolpath(_L_path(), MACHINE_DEFAULT, profile=KNIFE, lift_height=3.0)
    # The within-path corner is a ramped pure-A pivot (one or more MICRO_JOG
    # segments, no XY) bracketed by Z raise/lower.
    idxs = [i for i, x in enumerate(s)
            if (x.flags & MICRO_JOG) and x.da != 0 and x.dx == 0 and x.dy == 0]
    assert idxs, "expected a corner pivot"
    spd = MACHINE_DEFAULT.a.steps_per_unit
    total_da = sum(s[i].da for i in idxs)
    assert approx(abs(total_da), 90 * spd, tol=2 * spd)       # ~90deg turn
    # A Z raise precedes the first pivot segment, a Z lower follows the last.
    first, last = idxs[0], idxs[-1]
    assert s[first - 1].flags & MICRO_LIFT and s[first - 1].dz != 0
    assert s[last + 1].flags & MICRO_LIFT and s[last + 1].dz != 0

def test_in_curve_cusp_lift_pivot():
    # A cubic with P1==P2 traces out-and-back along the x-axis: B'(t)=0 at t=0.5,
    # a 180deg cusp INSIDE one curve. The blade must NOT pivot 180 while down;
    # the turn is deferred to a lift-pivot.
    from config import KNIFE
    from stage6 import build_toolpath
    from mock_stage6 import make_planned
    cusp = make_planned((0, 0), (10, 0), (10, 0), (0, 0),
                        0, 30, 0, flags=PATH_START | PATH_END)
    s = build_toolpath([cusp], MACHINE_DEFAULT, profile=KNIFE, lift_height=3.0)
    spd = MACHINE_DEFAULT.a.steps_per_unit
    # no pen-down segment carries a large rotation
    big_down = [x for x in s if not (x.flags & (MICRO_JOG | MICRO_LIFT))
                and abs(x.da) > 20 * spd]
    assert not big_down, "cusp rotation must not be tracked pen-down"
    # a pure-A lift-pivot appears (the deferred turn)
    pivots = [x for x in s if (x.flags & MICRO_JOG) and x.da != 0
              and x.dx == 0 and x.dy == 0]
    assert pivots, "expected a lift-pivot for the in-curve cusp"

def _square_subpath(x0, y0):
    # CCW square as four straight cubics; net tangent winds +360 around it.
    from mock_stage6 import make_planned
    def line(ax, ay, bx, by, flags):
        return make_planned((ax, ay), ((2*ax+bx)/3, (2*ay+by)/3),
                            ((ax+2*bx)/3, (ay+2*by)/3), (bx, by), 0, 30, 0, flags)
    return [
        line(x0,    y0,    x0+10, y0,    PATH_START),
        line(x0+10, y0,    x0+10, y0+10, 0),
        line(x0+10, y0+10, x0,    y0+10, 0),
        line(x0,    y0+10, x0,    y0,    PATH_END),
    ]

def _peak_wind_deg(segs):
    inv = -1 if MACHINE_DEFAULT.a.invert else 1
    spd = MACHINE_DEFAULT.a.steps_per_unit
    w = pk = 0.0
    for s in segs:
        w += s.da * inv / spd
        pk = max(pk, abs(w))
    return pk

def test_unwind_bounds_winding():
    # Two same-direction closed contours each wind +360. With unwind the wire
    # twist stays bounded (~one turn); without it, it grows per contour.
    from config import KNIFE
    from stage6 import build_toolpath
    from dataclasses import replace
    job = _square_subpath(0, 0) + _square_subpath(50, 0)
    on  = build_toolpath(job, MACHINE_DEFAULT, profile=KNIFE, lift_height=3.0)
    off = build_toolpath(job, MACHINE_DEFAULT,
                         profile=replace(KNIFE, name="free", unwind=False),
                         lift_height=3.0)
    assert _peak_wind_deg(on) < _peak_wind_deg(off)   # unwind helps
    assert _peak_wind_deg(on) <= 560                  # bounded ~one turn + entry

def test_corner_no_pivot_when_smooth():
    # A straight two-curve chain (no tangent jump) gets no mid-path corner pivot.
    # Pre-orientation (a pure-A ramp) may run at the start; once XY drawing
    # begins, no further pure-A pivot should appear.
    from config import KNIFE
    from stage6 import build_toolpath
    case = CASES["two_curve_chain"]
    s = build_toolpath(case["planned"], MACHINE_DEFAULT, profile=KNIFE, lift_height=3.0)
    first_draw = next(i for i, x in enumerate(s) if x.dx or x.dy)
    post = [x for x in s[first_draw:]
            if (x.flags & MICRO_JOG) and x.da != 0 and x.dx == 0 and x.dy == 0]
    assert not post, "no mid-path corner pivot on a straight chain"

# ── near-zero velocity: no crash, finite intervals ───────────────────────────

def test_near_zero_no_crash():
    s = segs("near_zero_velocity")
    assert len(s) > 0

def test_near_zero_finite_intervals():
    case = CASES["near_zero_velocity"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    max_allowed = case["expected"]["interval_max"]
    assert all(x.interval <= max_allowed for x in s)

def test_near_zero_no_zero_interval():
    s = segs("near_zero_velocity")
    assert all(x.interval > 0 for x in s)

# ── two-curve chain: total steps ──────────────────────────────────────────────

def test_chain_total_steps():
    case = CASES["two_curve_chain"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    total = sum(x.dx for x in s)
    assert approx(total, case["expected"]["total_steps_approx"], tol=10)

# ── MICRO_PATH_END flag ───────────────────────────────────────────────────────

def test_path_end_flag_set_on_last():
    s = segs("straight_constant_v")
    assert s[-1].flags & MICRO_PATH_END

def test_path_end_flag_not_on_others():
    s = segs("straight_constant_v")
    assert all((x.flags & MICRO_PATH_END) == 0 for x in s[:-1])

# ── dz always zero (no lift stage yet) ───────────────────────────────────────

def test_dz_always_zero():
    for name in CASES:
        s = segs(name)
        assert all(x.dz == 0 for x in s), f"{name}: non-zero dz"

# ── real SVG regression ───────────────────────────────────────────────────────

def test_snake_svg_segments():
    curves, _ = load_svg_mm(svg("test_snake.svg"))
    repaired, _ = enforce_c1(curves)
    metrics = compute_metrics(repaired)
    flags = [PATH_START] + [0] * (len(metrics) - 2) + [PATH_END]
    planned = plan_velocities(metrics, flags, 80.0, 1000.0)
    s = evaluate_microsegments(planned, MACHINE_DEFAULT)
    assert len(s) > 0
    # net x displacement: snake goes from x=10 to x=130 → 120mm → 9600 steps
    total_dx = sum(x.dx for x in s)
    assert approx(total_dx, 9600, tol=10)
    # last segment should be MICRO_PATH_END
    assert s[-1].flags & MICRO_PATH_END

if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except Exception as e:
            print(f"  FAIL  {t.__name__}: {e}")
            failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(failed)
