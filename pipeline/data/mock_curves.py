"""
Mock curve fixtures for the pipeline tests (flatten / constrain / plan).
Each case is a list of CubicBezier namedtuples in mm coordinates, as produced by
stage 3, paired with analytically-known arc length / curvature where possible so
tests can assert real numbers.
"""

import sys, os, math
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
from host.production.parse import CubicBezier

def pt(x, y):
    return (float(x), float(y))

# ── case 1: straight line ─────────────────────────────────────────────────────
# Degenerate cubic, control points collinear.
# Expected arc length: 100.0 mm exactly.
# Expected curvature:  0.0 everywhere.

STRAIGHT_LINE = [
    CubicBezier(pt(0, 0), pt(33.333, 0), pt(66.667, 0), pt(100, 0)),
]

# ── case 2: quarter circle radius 50mm ───────────────────────────────────────
# Standard cubic Bezier approximation of a quarter circle.
# Handle length = r * (4/3) * tan(π/8) ≈ r * 0.5522847
# Expected arc length: π * 50 / 2 ≈ 78.540 mm  (error <0.1%)
# Expected curvature:  ≈ 1/50 = 0.020 mm⁻¹ throughout

_R50 = 50.0
_H50 = _R50 * 0.5522847

QUARTER_CIRCLE_R50 = [
    CubicBezier(pt(_R50, 0), pt(_R50, _H50), pt(_H50, _R50), pt(0, _R50)),
]

# ── case 3: tight quarter circle radius 5mm ───────────────────────────────────
# Same shape, 10× smaller. High curvature case.
# Expected arc length: π * 5 / 2 ≈ 7.854 mm
# Expected curvature:  ≈ 1/5 = 0.200 mm⁻¹

_R5 = 5.0
_H5 = _R5 * 0.5522847

QUARTER_CIRCLE_R5 = [
    CubicBezier(pt(_R5, 0), pt(_R5, _H5), pt(_H5, _R5), pt(0, _R5)),
]

# ── case 4: S-curve ───────────────────────────────────────────────────────────
# Two opposing arcs. Curvature changes sign at the inflection point.
# kappa_max should be similar on both halves; overall path length ~arc of each.
# No clean analytical value — used to verify kappa sign change is detected.

S_CURVE = [
    CubicBezier(pt(0, 0),   pt(20, 40),  pt(40, 40),  pt(60, 0)),
    CubicBezier(pt(60, 0),  pt(80, -40), pt(100, -40), pt(120, 0)),
]

# ── case 5: very short curve (1mm) ────────────────────────────────────────────
# Tests numerical stability at near-degenerate scale.
# Expected arc length: ≈ 1.0 mm

SHORT_CURVE = [
    CubicBezier(pt(0, 0), pt(0.333, 0), pt(0.667, 0), pt(1.0, 0)),
]

# ── case 6: long gentle arc ───────────────────────────────────────────────────
# Low curvature throughout, long path. Tests that Gauss-Legendre handles
# a slow-bending 500mm curve without error accumulation.
# Rough expected arc length: slightly > 500mm (curve bows out by 50mm at peak).

LONG_GENTLE_ARC = [
    CubicBezier(pt(0, 0), pt(166.667, 50), pt(333.333, 50), pt(500, 0)),
]

# ── case 7: near-cusp (very high curvature) ───────────────────────────────────
# Control points arranged so the curve nearly doubles back on itself.
# kappa_max should be very large (small radius of curvature).

NEAR_CUSP = [
    CubicBezier(pt(0, 0), pt(40, 0), pt(41, 1), pt(1, 1)),
]

# ── case 8: full circle (four quarter arcs) ───────────────────────────────────
# Four quarter-circle cubics joined into a full circle of radius 30mm.
# Expected total arc length: 2 * π * 30 ≈ 188.496 mm
# Expected curvature: ≈ 1/30 ≈ 0.0333 mm⁻¹ throughout

_R30 = 30.0
_H30 = _R30 * 0.5522847

FULL_CIRCLE_R30 = [
    CubicBezier(pt(_R30, 0),   pt(_R30, _H30),  pt(_H30, _R30),   pt(0, _R30)),
    CubicBezier(pt(0, _R30),   pt(-_H30, _R30), pt(-_R30, _H30),  pt(-_R30, 0)),
    CubicBezier(pt(-_R30, 0),  pt(-_R30, -_H30),pt(-_H30, -_R30), pt(0, -_R30)),
    CubicBezier(pt(0, -_R30),  pt(_H30, -_R30), pt(_R30, -_H30),  pt(_R30, 0)),
]

# ── registry ──────────────────────────────────────────────────────────────────

CASES = {
    "straight_line":         (STRAIGHT_LINE,        {"arc_length": 100.0,            "kappa_max": 0.0}),
    "quarter_circle_r50":    (QUARTER_CIRCLE_R50,   {"arc_length": math.pi*50/2,     "kappa_max": 1/50}),
    "quarter_circle_r5":     (QUARTER_CIRCLE_R5,    {"arc_length": math.pi*5/2,      "kappa_max": 1/5}),
    "s_curve":               (S_CURVE,              {"arc_length": None,             "kappa_max": None}),
    "short_curve":           (SHORT_CURVE,          {"arc_length": 1.0,              "kappa_max": 0.0}),
    "long_gentle_arc":       (LONG_GENTLE_ARC,      {"arc_length": None,             "kappa_max": None}),
    "near_cusp":             (NEAR_CUSP,            {"arc_length": None,             "kappa_max": None}),
    "full_circle_r30":       (FULL_CIRCLE_R30,      {"arc_length": 2*math.pi*30,     "kappa_max": 1/30}),
}

if __name__ == "__main__":
    for name, (curves, expected) in CASES.items():
        print(f"{name} ({len(curves)} curve(s))  expected: {expected}")
        for i, c in enumerate(curves):
            print(f"  [{i}] p0={c.p0}  p3={c.p3}")
        print()
