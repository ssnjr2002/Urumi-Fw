"""Tests for stage 3: C1 continuity enforcement."""

import sys, os, math
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
DATA = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "pipeline", "data"))

from host.production.repair import enforce_c1, _exit_tangent, _entry_tangent, _angle_between_deg
from pipeline.data.mock_stage3 import CASES

def approx(a, b, tol=1e-6):
    return all(abs(x - y) < tol for x, y in zip(a, b))

# ── passthrough cases ─────────────────────────────────────────────────────────

def test_perfect_c1_untouched():
    repaired, logs = enforce_c1(CASES["c1_perfect"])
    assert len(repaired) == 2 and len(logs) == 0

def test_g1_not_c1_untouched():
    # direction is continuous — stage 3 should not insert anything
    repaired, logs = enforce_c1(CASES["g1_not_c1"])
    assert len(repaired) == 2 and len(logs) == 0

def test_near_c1_within_tolerance_untouched():
    repaired, logs = enforce_c1(CASES["near_c1_within_tolerance"])
    assert len(repaired) == 2 and len(logs) == 0

def test_single_curve_untouched():
    repaired, logs = enforce_c1(CASES["single_curve"])
    assert len(repaired) == 1 and len(logs) == 0

def test_empty_list():
    repaired, logs = enforce_c1([])
    assert repaired == [] and logs == []

# ── repair cases ──────────────────────────────────────────────────────────────

def test_sharp_corner_logged_as_cusp():
    # Zero-gap sharp corners are logged but not modified — no loop inserted
    repaired, logs = enforce_c1(CASES["c0_only_sharp_corner"])
    assert len(repaired) == 2   # original curves unchanged
    assert len(logs) == 1
    assert logs[0].kind == "cusp"
    assert abs(logs[0].angle_deg - 90.0) < 0.1

def test_gap_inserts_bridge():
    repaired, logs = enforce_c1(CASES["c0_broken_gap"])
    assert len(repaired) == 3
    assert len(logs) == 1
    assert logs[0].kind == "bridge"
    assert logs[0].gap_mm > 0.01

def test_multi_bad_joins_both_logged():
    # Zero-gap sharp corners are now logged as cusps, not modified
    repaired, logs = enforce_c1(CASES["multi_bad_joins"])
    assert len(repaired) == 3   # no extra curves inserted
    assert len(logs) == 2
    assert all(l.kind == "cusp" for l in logs)

def test_cusp_logged_not_modified():
    repaired, logs = enforce_c1(CASES["cusp"])
    assert len(repaired) == 2   # no blend cubic inserted
    assert len(logs) == 1
    assert logs[0].kind == "cusp"
    assert abs(logs[0].angle_deg - 180.0) < 0.1

# ── blend cubic geometry ──────────────────────────────────────────────────────

def test_blend_endpoints_preserved():
    # original curve endpoints must be unchanged after bridge insertion
    repaired, _ = enforce_c1(CASES["c0_broken_gap"])
    original = CASES["c0_broken_gap"]
    assert approx(repaired[0].p0, original[0].p0)
    assert approx(repaired[0].p3, original[0].p3)
    assert approx(repaired[2].p0, original[1].p0)
    assert approx(repaired[2].p3, original[1].p3)

def test_blend_connects_at_join():
    # bridge cubic p0 == curve[0].p3, bridge p3 == curve[1].p0
    repaired, _ = enforce_c1(CASES["c0_broken_gap"])
    bridge = repaired[1]
    assert approx(bridge.p0, repaired[0].p3)
    assert approx(bridge.p3, repaired[2].p0)

def test_blend_respects_exit_tangent():
    # bridge p1 must lie along the exit tangent of the preceding curve
    repaired, _ = enforce_c1(CASES["c0_broken_gap"])
    exit_t = _exit_tangent(repaired[0])
    bridge = repaired[1]
    handle = (bridge.p1[0] - bridge.p0[0], bridge.p1[1] - bridge.p0[1])
    l = math.sqrt(handle[0]**2 + handle[1]**2) or 1
    angle = _angle_between_deg(exit_t, (handle[0]/l, handle[1]/l))
    assert angle < 1.0

def test_blend_respects_entry_tangent():
    # bridge p2 must lie along the entry tangent of the following curve
    repaired, _ = enforce_c1(CASES["c0_broken_gap"])
    entry_t = _entry_tangent(repaired[2])
    bridge = repaired[1]
    handle = (bridge.p3[0] - bridge.p2[0], bridge.p3[1] - bridge.p2[1])
    l = math.sqrt(handle[0]**2 + handle[1]**2) or 1
    angle = _angle_between_deg(entry_t, (handle[0]/l, handle[1]/l))
    assert angle < 1.0

# ── angle tolerance boundary ──────────────────────────────────────────────────

def test_exactly_at_tolerance_not_repaired():
    repaired, logs = enforce_c1(CASES["near_c1_within_tolerance"], angle_tol_deg=5.0)
    assert len(logs) == 0

def test_tighter_tolerance_triggers_repair():
    repaired, logs = enforce_c1(CASES["near_c1_within_tolerance"], angle_tol_deg=2.0)
    assert len(logs) == 1

# ── real SVG passthrough ──────────────────────────────────────────────────────

def test_snake_svg_no_repairs():
    from host.production.normalise import load_svg_mm
    curves, _ = load_svg_mm(os.path.join(DATA, "test_snake.svg"))
    repaired, logs = enforce_c1(curves)
    assert len(logs) == 0
    assert len(repaired) == len(curves)

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
