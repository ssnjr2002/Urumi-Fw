"""Tests for stage 4: arc length + curvature computation."""

import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from stage4 import compute_metrics, arc_length, curvature, kappa_samples, CurveMetrics
from stage2 import load_svg_mm
from stage3 import enforce_c1
from mock_stage4 import CASES

DATA = os.path.join(os.path.dirname(__file__), "..", "data")

def svg(name):
    return os.path.join(DATA, name)

# ── arc length ────────────────────────────────────────────────────────────────

def test_straight_line_length():
    curves, expected = CASES["straight_line"]
    m = compute_metrics(curves)
    assert abs(m[0].path_length_mm - expected["arc_length"]) < 0.001

def test_quarter_circle_r50_length():
    curves, expected = CASES["quarter_circle_r50"]
    m = compute_metrics(curves)
    err = abs(m[0].path_length_mm - expected["arc_length"]) / expected["arc_length"]
    assert err < 0.001  # < 0.1%

def test_quarter_circle_r5_length():
    curves, expected = CASES["quarter_circle_r5"]
    m = compute_metrics(curves)
    err = abs(m[0].path_length_mm - expected["arc_length"]) / expected["arc_length"]
    assert err < 0.001

def test_short_curve_length():
    curves, expected = CASES["short_curve"]
    m = compute_metrics(curves)
    assert abs(m[0].path_length_mm - expected["arc_length"]) < 0.0001

def test_full_circle_total_length():
    # sum of four quarter arcs must match 2*pi*r within 0.1%
    curves, expected = CASES["full_circle_r30"]
    metrics = compute_metrics(curves)
    total = sum(m.path_length_mm for m in metrics)
    err = abs(total - expected["arc_length"]) / expected["arc_length"]
    assert err < 0.001

def test_length_positive():
    for name, (curves, _) in CASES.items():
        for m in compute_metrics(curves):
            assert m.path_length_mm > 0, f"{name}: non-positive length"

# ── curvature ─────────────────────────────────────────────────────────────────

def test_straight_line_zero_curvature():
    curves, _ = CASES["straight_line"]
    m = compute_metrics(curves)
    assert m[0].kappa_max < 1e-6

def test_short_curve_zero_curvature():
    curves, _ = CASES["short_curve"]
    m = compute_metrics(curves)
    assert m[0].kappa_max < 1e-6

def test_quarter_circle_r50_curvature():
    # kappa should be close to 1/r = 0.02 throughout
    curves, expected = CASES["quarter_circle_r50"]
    m = compute_metrics(curves)
    err = abs(m[0].kappa_max - expected["kappa_max"]) / expected["kappa_max"]
    assert err < 0.02  # within 2%

def test_quarter_circle_r5_curvature():
    curves, expected = CASES["quarter_circle_r5"]
    m = compute_metrics(curves)
    err = abs(m[0].kappa_max - expected["kappa_max"]) / expected["kappa_max"]
    assert err < 0.02

def test_tighter_circle_higher_curvature():
    # r5 should have 10x higher curvature than r50
    m50 = compute_metrics(CASES["quarter_circle_r50"][0])
    m5  = compute_metrics(CASES["quarter_circle_r5"][0])
    ratio = m5[0].kappa_max / m50[0].kappa_max
    assert abs(ratio - 10.0) < 0.5

def test_near_cusp_high_curvature():
    curves, _ = CASES["near_cusp"]
    m = compute_metrics(curves)
    assert m[0].kappa_max > 1.0  # clearly high

def test_full_circle_curvature():
    curves, expected = CASES["full_circle_r30"]
    metrics = compute_metrics(curves)
    for m in metrics:
        err = abs(m.kappa_max - expected["kappa_max"]) / expected["kappa_max"]
        assert err < 0.02

# ── kappa_samples structure ───────────────────────────────────────────────────

def test_kappa_samples_count():
    curves, _ = CASES["quarter_circle_r50"]
    m = compute_metrics(curves)
    assert len(m[0].kappa_samples) == 20

def test_kappa_samples_t_range():
    curves, _ = CASES["s_curve"]
    m = compute_metrics(curves)
    for curve_m in m:
        ts = [t for t, _ in curve_m.kappa_samples]
        assert abs(ts[0]  - 0.0) < 1e-9
        assert abs(ts[-1] - 1.0) < 1e-9

def test_kappa_samples_nonnegative():
    for name, (curves, _) in CASES.items():
        for m in compute_metrics(curves):
            for t, k in m.kappa_samples:
                assert k >= 0, f"{name}: negative curvature at t={t}"

def test_kappa_max_equals_sample_max():
    for name, (curves, _) in CASES.items():
        for m in compute_metrics(curves):
            sample_max = max(k for _, k in m.kappa_samples)
            assert abs(m.kappa_max - sample_max) < 1e-12, f"{name}: kappa_max mismatch"

# ── output structure ──────────────────────────────────────────────────────────

def test_returns_curve_metrics():
    curves, _ = CASES["straight_line"]
    m = compute_metrics(curves)
    assert isinstance(m[0], CurveMetrics)
    assert m[0].curve == curves[0]

def test_output_length_matches_input():
    for name, (curves, _) in CASES.items():
        m = compute_metrics(curves)
        assert len(m) == len(curves), f"{name}: output length mismatch"

# ── real SVG regression ───────────────────────────────────────────────────────

def test_snake_svg_metrics():
    curves, _ = load_svg_mm(svg("test_snake.svg"))
    repaired, _ = enforce_c1(curves)
    metrics = compute_metrics(repaired)
    assert len(metrics) == 2
    total = sum(m.path_length_mm for m in metrics)
    assert 150 < total < 200  # sanity range for a 130mm-wide snake in a 150x100 viewBox

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
