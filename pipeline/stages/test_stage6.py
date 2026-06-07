"""Tests for stage 6: Bezier evaluation -> MicroSegments."""

import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from stage6 import evaluate_microsegments, MicroSegment, MICRO_PATH_END, V_MIN
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
    s = segs("diagonal_constant_v")
    assert all(x.da == 0 for x in s)

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
    case = CASES["arc_tangent_tracking"]
    s = evaluate_microsegments(case["planned"], case["machine"])
    total_da = sum(x.da for x in s)
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
