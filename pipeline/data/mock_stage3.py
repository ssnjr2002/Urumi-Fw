"""
Mock input data for stage 3 (C1 continuity enforcement).
Each case is a list of CubicBezier namedtuples in mm coordinates,
as produced by stage 2.
"""

import sys, os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "stages"))
from stage1 import CubicBezier

# ── helpers ───────────────────────────────────────────────────────────────────

def pt(x, y):
    return (float(x), float(y))

# ── case 1: perfect C1 join ───────────────────────────────────────────────────
# p2 of [0], p3 of [0] / p0 of [1], p1 of [1] are collinear AND equidistant.
# Stage 3 should pass this through untouched.

C1_PERFECT = [
    CubicBezier(pt(0,0),   pt(10,20),  pt(20,30),  pt(30,30)),
    CubicBezier(pt(30,30), pt(40,30),  pt(50,20),  pt(60,0)),
    #            p0          p1          p2           p3
    # join: p2=[0]=(20,30), p3=[0]=p0=[1]=(30,30), p1=[1]=(40,30)
    # all y=30, x evenly spaced by 10 — collinear and equidistant. ✓
]

# ── case 2: C0 only, tangent direction break ──────────────────────────────────
# Endpoints match but the tangent turns sharply at the join (~90°).
# Stage 3 must insert a blending cubic.

C0_ONLY_SHARP_CORNER = [
    CubicBezier(pt(0,0),   pt(10,0),   pt(20,0),   pt(30,0)),
    CubicBezier(pt(30,0),  pt(30,10),  pt(30,20),  pt(30,30)),
    # exit tangent of [0]: rightward (+x)
    # entry tangent of [1]: upward (+y)
    # angle break = 90° — well above any tolerance
]

# ── case 3: G1 but not C1 (collinear, unequal speed) ─────────────────────────
# Direction is continuous but the handle lengths differ.
# Strict C1 requires equal distances; G1 (direction only) does not.
# Flagged here so stage 3 can decide its tolerance policy.

G1_NOT_C1 = [
    CubicBezier(pt(0,0),   pt(10,0),   pt(20,0),   pt(30,0)),
    CubicBezier(pt(30,0),  pt(50,0),   pt(60,0),   pt(70,0)),
    # exit handle  [0]: p3-p2 = (10,0), length=10
    # entry handle [1]: p1-p0 = (20,0), length=20
    # direction identical (both +x) but speed doubles at join
]

# ── case 4: position gap (C0 broken) ─────────────────────────────────────────
# p3 of [0] != p0 of [1] — there is a spatial gap.
# Stage 3 should detect and report this (insert a bridging move or raise).

C0_BROKEN_GAP = [
    CubicBezier(pt(0,0),   pt(10,0),   pt(20,0),   pt(30,0)),
    CubicBezier(pt(40,10), pt(50,10),  pt(60,10),  pt(70,10)),
    # gap: (30,0) -> (40,10), distance ~14mm
]

# ── case 5: multiple consecutive bad joins ────────────────────────────────────
# Three curves, two joins both broken. Tests that repair iterates correctly.

MULTI_BAD_JOINS = [
    CubicBezier(pt(0,0),   pt(10,0),   pt(20,0),   pt(30,0)),   # exits rightward
    CubicBezier(pt(30,0),  pt(30,10),  pt(30,20),  pt(30,30)),  # exits upward
    CubicBezier(pt(30,30), pt(20,30),  pt(10,30),  pt(0,30)),   # exits leftward
    # join 0-1: 90° break
    # join 1-2: 90° break
]

# ── case 6: single curve (no join to check) ───────────────────────────────────
# Stage 3 must handle a length-1 list without crashing.

SINGLE_CURVE = [
    CubicBezier(pt(0,0), pt(10,20), pt(20,20), pt(30,0)),
]

# ── case 7: near-C1 within tolerance ─────────────────────────────────────────
# Tangent angle deviation is small (~2°) — below the 5° threshold.
# Stage 3 should pass this through without inserting a blend.

NEAR_C1_WITHIN_TOLERANCE = [
    CubicBezier(pt(0,0),   pt(10,0),        pt(20,0),        pt(30,0)),
    CubicBezier(pt(30,0),  pt(40, 0.698),   pt(50, 0.698),   pt(60,0)),
    # p1 of [1] is (40, 0.698): angle from (30,0) ≈ atan2(0.698,10) ≈ 4°
]

# ── case 8: anti-parallel tangents (180° — a cusp) ───────────────────────────
# Exit tangent and entry tangent point in exactly opposite directions.
# The curve doubles back on itself — worst-case for the repair logic.

CUSP = [
    CubicBezier(pt(0,0),   pt(10,0),  pt(20,0),  pt(30,0)),
    CubicBezier(pt(30,0),  pt(20,0),  pt(10,0),  pt(0,0)),
    # entry tangent of [1]: p1-p0 = (-10,0) — directly opposite exit tangent
]

# ── registry ──────────────────────────────────────────────────────────────────

CASES = {
    "c1_perfect":              C1_PERFECT,
    "c0_only_sharp_corner":    C0_ONLY_SHARP_CORNER,
    "g1_not_c1":               G1_NOT_C1,
    "c0_broken_gap":           C0_BROKEN_GAP,
    "multi_bad_joins":         MULTI_BAD_JOINS,
    "single_curve":            SINGLE_CURVE,
    "near_c1_within_tolerance":NEAR_C1_WITHIN_TOLERANCE,
    "cusp":                    CUSP,
}

if __name__ == "__main__":
    for name, curves in CASES.items():
        print(f"{name} ({len(curves)} curve(s)):")
        for i, c in enumerate(curves):
            print(f"  [{i}] p0={c.p0}  p1={c.p1}  p2={c.p2}  p3={c.p3}")
        print()
