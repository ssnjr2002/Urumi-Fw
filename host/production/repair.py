"""
Stage 3: C1 continuity enforcement at curve joins.
For each join between curve[i] and curve[i+1]:
  - Check C0: endpoints meet (within gap_tol)
  - Check G1: exit tangent of [i] parallel to entry tangent of [i+1]
    (direction only — not speed, per plan section 5.1)
  - If angle deviation > angle_tol: insert a blending cubic
  - If C0 gap > gap_tol: log a warning and insert a bridging line
Output: repaired curve list + list of RepairLog entries.
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
from host.production.parse import CubicBezier
from host.production.normalise import load_svg_mm
from pipeline.config import default as _config_default
from collections import namedtuple

RepairLog = namedtuple("RepairLog", ["join_index", "kind", "angle_deg", "gap_mm"])
# kind: "blend" | "bridge" | "cusp"

# ── geometry helpers ──────────────────────────────────────────────────────────

def _sub(a, b):
    return (a[0] - b[0], a[1] - b[1])

def _add(a, b):
    return (a[0] + b[0], a[1] + b[1])

def _scale(v, s):
    return (v[0] * s, v[1] * s)

def _length(v):
    return math.sqrt(v[0]*v[0] + v[1]*v[1])

def _normalize(v):
    l = _length(v)
    if l < 1e-12:
        return (0.0, 0.0)
    return (v[0] / l, v[1] / l)

def _angle_between_deg(u, v):
    """Signed angle from u to v in degrees, range [0, 180]."""
    dot = u[0]*v[0] + u[1]*v[1]
    dot = max(-1.0, min(1.0, dot))
    return math.degrees(math.acos(dot))

def _exit_tangent(c):
    """Unit tangent leaving curve c (direction p2 -> p3)."""
    return _normalize(_sub(c.p3, c.p2))

def _entry_tangent(c):
    """Unit tangent entering curve c (direction p0 -> p1)."""
    return _normalize(_sub(c.p1, c.p0))

# ── blending cubic ────────────────────────────────────────────────────────────

_MIN_HANDLE_MM = 1.0  # minimum handle length for zero-gap blends

def _blend_cubic(p0, exit_tan, p3, entry_tan):
    """
    Insert a short cubic from p0 to p3 that respects the tangent directions
    on both sides. Handle length = 1/3 of chord, minimum 1mm so tangents
    are preserved even when p0==p3 (zero-gap corner blend).
    """
    chord = _length(_sub(p3, p0))
    h = max(chord / 3.0, _MIN_HANDLE_MM)
    p1 = _add(p0, _scale(exit_tan,  h))
    p2 = _add(p3, _scale(entry_tan, -h))
    return CubicBezier(p0, p1, p2, p3)

# ── main stage ────────────────────────────────────────────────────────────────

def enforce_c1(curves, angle_tol_deg=None, gap_tol_mm=None):
    """
    Returns (repaired_curves, logs).
    repaired_curves: original curves with blending cubics inserted at bad joins.
    logs: list of RepairLog for every join that needed intervention.
    Tolerances default to config.default().quality when not given.
    """
    if angle_tol_deg is None or gap_tol_mm is None:
        _q = _config_default().quality
        if angle_tol_deg is None: angle_tol_deg = _q.angle_tol
        if gap_tol_mm   is None: gap_tol_mm   = _q.gap_tol

    if len(curves) <= 1:
        return list(curves), []

    repaired = [curves[0]]
    logs = []

    for i in range(len(curves) - 1):
        a = curves[i]
        b = curves[i + 1]

        gap = _length(_sub(b.p0, a.p3))
        exit_t  = _exit_tangent(a)
        entry_t = _entry_tangent(b)

        # degenerate tangents (zero-length handle) — treat as cusp
        if _length(exit_t) < 0.5 or _length(entry_t) < 0.5:
            logs.append(RepairLog(i, "cusp", 180.0, gap))
            blend = _blend_cubic(a.p3, (1, 0), b.p0, (1, 0))  # fallback direction
            repaired.append(blend)
            repaired.append(b)
            continue

        angle = _angle_between_deg(exit_t, entry_t)

        if gap > gap_tol_mm:
            # C0 broken: bridge the gap
            logs.append(RepairLog(i, "bridge", angle, gap))
            bridge = _blend_cubic(a.p3, exit_t, b.p0, entry_t)
            repaired.append(bridge)

        elif angle > angle_tol_deg:
            # Sharp corner at a shared point — log as cusp, leave as-is.
            # Inserting a blend here produces a tiny loop (p0==p3) which a
            # plotter would trace. Velocity planning handles cornering instead.
            logs.append(RepairLog(i, "cusp", angle, gap))

        repaired.append(b)

    return repaired, logs

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    _q = _config_default().quality
    parser = argparse.ArgumentParser(description="Stage 3: C1 continuity enforcement")
    parser.add_argument("svg", help="Path to SVG file")
    parser.add_argument("--angle-tol", type=float, default=_q.angle_tol, help="Angle tolerance in degrees")
    parser.add_argument("--gap-tol",   type=float, default=_q.gap_tol, help="Gap tolerance in mm")
    args = parser.parse_args()

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, logs = enforce_c1(curves_mm, args.angle_tol, args.gap_tol)

    print(f"Input:    {len(curves_mm)} curve(s)")
    print(f"Output:   {len(repaired)} curve(s)  ({len(repaired) - len(curves_mm)} inserted)")
    print(f"Repairs:  {len(logs)}\n")
    for log in logs:
        print(f"  join {log.join_index}: {log.kind:8s}  angle={log.angle_deg:.1f}°  gap={log.gap_mm:.4f}mm")
    if not logs:
        print("  (none — all joins already continuous)")
