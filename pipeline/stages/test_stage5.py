"""Tests for stage 5: velocity planner."""

import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from stage5 import plan_velocities, PlannedCurve, PATH_START, PATH_END, MERGE_WITH_PREV
from stage2 import load_svg_mm
from stage3 import enforce_c1
from stage4 import compute_metrics
from mock_stage5 import CASES

DATA = os.path.join(os.path.dirname(__file__), "..", "data")

def svg(name):
    return os.path.join(DATA, name)

def approx(a, b, tol=1.0):
    return abs(a - b) < tol

# ── boundary conditions ───────────────────────────────────────────────────────

def test_path_start_zero_entry():
    p = plan_velocities(**{k: v for k, v in CASES["single_start_end"].items() if k != "expected"})
    assert p[0].v_entry == 0.0

def test_path_end_zero_exit():
    p = plan_velocities(**{k: v for k, v in CASES["single_start_end"].items() if k != "expected"})
    assert p[-1].v_exit == 0.0

def test_two_paths_independent():
    # Each path resets to v=0; velocities must not bleed across
    p = plan_velocities(**{k: v for k, v in CASES["two_paths"].items() if k != "expected"})
    assert p[0].v_entry == 0.0 and p[0].v_exit == 0.0
    assert p[1].v_entry == 0.0 and p[1].v_exit == 0.0

# ── cruise behaviour ──────────────────────────────────────────────────────────

def test_long_straight_reaches_feed_max():
    case = CASES["long_straight_cruise"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    assert approx(p[0].v_cruise, case["feed_max"], tol=1.0)

def test_chain_middle_cruises():
    p = plan_velocities(**{k: v for k, v in CASES["chain_three"].items() if k != "expected"})
    assert p[1].v_entry > 0
    assert p[1].v_cruise > 0
    assert approx(p[1].v_entry, p[1].v_cruise, tol=1.0)

def test_chain_endpoints_zero():
    p = plan_velocities(**{k: v for k, v in CASES["chain_three"].items() if k != "expected"})
    assert p[0].v_entry == 0.0
    assert p[-1].v_exit == 0.0

# ── triangular profile ────────────────────────────────────────────────────────

def test_short_curve_triangular():
    case = CASES["short_triangular"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    assert p[0].v_cruise < case["feed_max"]

def test_short_curve_peak_correct():
    # v_peak = sqrt(a_max * length) for symmetric triangular (v_entry=v_exit=0)
    case = CASES["short_triangular"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    expected_peak = math.sqrt(case["a_max"] * case["metrics"][0].path_length_mm)
    assert approx(p[0].v_cruise, expected_peak, tol=1.0)

# ── centripetal limit ─────────────────────────────────────────────────────────

def test_tight_corner_capped():
    case = CASES["tight_corner"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    corner = p[1]
    v_cap = math.sqrt(case["a_max"] / corner.metrics.kappa_max)
    assert approx(corner.v_cruise, v_cap, tol=1.0)

def test_tight_corner_below_feed_max():
    case = CASES["tight_corner"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    assert p[1].v_cruise < case["feed_max"]

def test_tight_corner_backward_tightens_approach():
    # curve[0] exit must be low enough to decelerate to corner speed
    case = CASES["tight_corner"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    v_corner = p[1].v_entry
    v_exit_before = p[0].v_exit
    d = case["metrics"][0].path_length_mm
    # check: deceleration from v_exit_before to v_corner is achievable in d
    v_check = math.sqrt(v_corner**2 + 2 * case["a_max"] * d)
    assert v_exit_before <= v_check + 1.0

# ── backward pass tightening ──────────────────────────────────────────────────

def test_backward_tighten_reduces_exit():
    case = CASES["backward_tighten"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    assert p[1].v_exit < case["feed_max"]

def test_backward_tighten_entry_approx():
    case = CASES["backward_tighten"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    expected = math.sqrt(2 * case["a_max"] * case["metrics"][2].path_length_mm)
    assert approx(p[2].v_entry, expected, tol=1.0)

# ── merge groups ──────────────────────────────────────────────────────────────

def test_merge_flags_set():
    p = plan_velocities(**{k: v for k, v in CASES["merge_two_short"].items() if k != "expected"})
    assert all(pc.merged for pc in p)

def test_merge_continuous_velocity():
    # v_exit of curve[0] should equal v_entry of curve[1]
    p = plan_velocities(**{k: v for k, v in CASES["merge_two_short"].items() if k != "expected"})
    assert approx(p[0].v_exit, p[1].v_entry, tol=0.01)

def test_merge_peak_uses_total_length():
    # v_peak = sqrt(a_max * total_length) for merged triangular profile
    case = CASES["merge_two_short"]
    p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
    total = sum(m.path_length_mm for m in case["metrics"])
    expected_peak = math.sqrt(case["a_max"] * total)
    assert approx(p[0].v_cruise, expected_peak, tol=1.0)

# ── output structure ──────────────────────────────────────────────────────────

def test_returns_planned_curve():
    p = plan_velocities(**{k: v for k, v in CASES["single_start_end"].items() if k != "expected"})
    assert isinstance(p[0], PlannedCurve)

def test_output_length_matches_input():
    for name, case in CASES.items():
        p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
        assert len(p) == len(case["metrics"]), f"{name}: length mismatch"

def test_velocities_nonnegative():
    for name, case in CASES.items():
        p = plan_velocities(**{k: v for k, v in case.items() if k != "expected"})
        for pc in p:
            assert pc.v_entry >= 0 and pc.v_cruise >= 0 and pc.v_exit >= 0, \
                f"{name}: negative velocity"

def test_empty_input():
    p = plan_velocities([], [], 100.0, 1000.0)
    assert p == []

# ── real SVG regression ───────────────────────────────────────────────────────

def test_snake_svg_planned():
    curves, _ = load_svg_mm(svg("test_snake.svg"))
    repaired, _ = enforce_c1(curves)
    metrics = compute_metrics(repaired)
    flags = [PATH_START] + [0] * (len(metrics) - 2) + [PATH_END]
    planned = plan_velocities(metrics, flags, 80.0, 1000.0)
    assert planned[0].v_entry == 0.0
    assert planned[-1].v_exit == 0.0
    assert all(p.v_cruise > 0 for p in planned)

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
