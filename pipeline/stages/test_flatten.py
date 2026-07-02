"""Tests for the Flatten stage (redesign stage 4): Bezier subpaths -> Sample stream.

Validates the sample stream reproduces stage4's geometry (arc length, endpoints,
curvature) — flatten is a re-representation, not a re-computation, so it must
agree with the trusted stage4 metrics.
"""

import sys, os, math
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
DATA = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "data"))

from pipeline.stages.flatten import flatten
from pipeline.stages.sample import Sample, PATH_START, PATH_END, CURVE_BOUNDARY
from pipeline.stages.bezier import arc_length
from host.production.normalise import load_svg_mm_subpaths
from host.production.repair import enforce_c1
from pipeline.data.mock_curves import CASES

def _analytic_len(curves):
    return sum(arc_length(c) for c in curves)


def svg(name):
    return os.path.join(DATA, name)

# ── arc length: sum of ds matches stage4 ──────────────────────────────────────

def test_total_length_matches_stage4():
    # The sum of per-sample ds must match stage4's analytic arc length closely.
    # Chord sampling under-measures slightly on curves; ds_max keeps it tight.
    # EXCEPT near_cusp: stage4's 5-point Gauss-Legendre under-estimates a sharp
    # near-cusp (it samples between the spike), so the dense chord sum is the more
    # accurate figure there, not the reference. Excluded from the parity check.
    for name, (curves, _) in CASES.items():
        if name == "near_cusp":
            continue
        samples = flatten([curves])
        chord_total = sum(s.ds for s in samples)
        analytic = _analytic_len(curves)
        err = abs(chord_total - analytic) / analytic
        assert err < 0.005, f"{name}: chord len {chord_total} vs analytic {analytic}"

def test_near_cusp_length_reasonable():
    # The dense chord sum should be a tad LONGER than stage4's GL5 (which misses
    # the cusp spike) — a sanity floor, not a parity assert.
    samples = flatten([CASES["near_cusp"][0]])
    chord_total = sum(s.ds for s in samples)
    gl5 = _analytic_len(CASES["near_cusp"][0])
    assert chord_total >= gl5 - 1e-6

def test_straight_line_length():
    samples = flatten([CASES["straight_line"][0]])
    assert abs(sum(s.ds for s in samples) - 100.0) < 0.01

# ── endpoints preserved ───────────────────────────────────────────────────────

def test_endpoints_match_curve():
    for name, (curves, _) in CASES.items():
        samples = flatten([curves])
        assert abs(samples[0].x - curves[0].p0[0]) < 1e-9, name
        assert abs(samples[0].y - curves[0].p0[1]) < 1e-9, name
        assert abs(samples[-1].x - curves[-1].p3[0]) < 1e-6, name
        assert abs(samples[-1].y - curves[-1].p3[1]) < 1e-6, name

# ── curvature parity with stage4 ──────────────────────────────────────────────

def test_circle_curvature_constant():
    # A quarter circle r50 -> kappa ~ 0.02 at every sample.
    samples = flatten([CASES["quarter_circle_r50"][0]])
    for s in samples:
        assert abs(s.kappa - 0.02) < 0.02 * 0.05  # within 5%

def test_straight_zero_curvature():
    samples = flatten([CASES["straight_line"][0]])
    assert max(s.kappa for s in samples) < 1e-6

# ── flags ─────────────────────────────────────────────────────────────────────

def test_path_start_end_flags():
    samples = flatten([CASES["s_curve"][0]])
    assert samples[0].flags & PATH_START
    assert samples[-1].flags & PATH_END
    # exactly one of each per subpath
    assert sum(1 for s in samples if s.flags & PATH_START) == 1
    assert sum(1 for s in samples if s.flags & PATH_END) == 1

def test_curve_boundary_flag():
    # s_curve has 2 curves -> exactly one CURVE_BOUNDARY (start of 2nd curve).
    samples = flatten([CASES["s_curve"][0]])
    assert sum(1 for s in samples if s.flags & CURVE_BOUNDARY) == 1

def test_full_circle_four_boundaries():
    # four quarter arcs -> 3 internal boundaries (first curve has no boundary).
    samples = flatten([CASES["full_circle_r30"][0]])
    assert sum(1 for s in samples if s.flags & CURVE_BOUNDARY) == 3

# ── multi-subpath ─────────────────────────────────────────────────────────────

def test_two_subpaths_bracketed():
    a = CASES["straight_line"][0]
    b = CASES["quarter_circle_r50"][0]
    samples = flatten([a, b])
    assert sum(1 for s in samples if s.flags & PATH_START) == 2
    assert sum(1 for s in samples if s.flags & PATH_END) == 2
    # ds does not bridge subpaths: the last sample of subpath A has ds=0
    starts = [i for i, s in enumerate(samples) if s.flags & PATH_START]
    assert samples[starts[1] - 1].ds == 0.0

# ── spacing cap ───────────────────────────────────────────────────────────────

def test_ds_max_respected():
    # No sample step exceeds ds_max (except the trailing 0). Use a long straight.
    from pipeline.stages.config import default
    q = default().quality
    samples = flatten([CASES["straight_line"][0]], quality=q)
    for s in samples[:-1]:
        assert s.ds <= q.ds_max + 1e-6

def test_corner_shows_as_tangent_jump():
    # A sharp 90-degree corner between two straight subcurves shows up as a
    # large theta jump across the CURVE_BOUNDARY, with ~zero ds.
    from host.production.parse import CubicBezier
    def line(p0, p1):
        d = ((p1[0]-p0[0])/3, (p1[1]-p0[1])/3)
        return CubicBezier(p0, (p0[0]+d[0], p0[1]+d[1]),
                              (p1[0]-d[0], p1[1]-d[1]), p1)
    horiz = line((0.0, 0.0), (10.0, 0.0))
    vert  = line((10.0, 0.0), (10.0, 10.0))
    samples = flatten([[horiz, vert]])
    bi = next(i for i, s in enumerate(samples) if s.flags & CURVE_BOUNDARY)
    jump = abs(samples[bi].theta - samples[bi - 1].theta)
    assert abs(jump - 90.0) < 1.0
    assert samples[bi - 1].ds < 1e-6  # shared corner point, ~zero gap

# ── real SVG regression ───────────────────────────────────────────────────────

def test_snake_svg_flatten():
    subpaths_mm, _ = load_svg_mm_subpaths(svg("test_snake.svg"))
    repaired = [enforce_c1(sp)[0] for sp in subpaths_mm]
    samples = flatten(repaired)
    total = sum(s.ds for s in samples)
    assert 150 < total < 200
    assert all(isinstance(s, Sample) for s in samples)

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
