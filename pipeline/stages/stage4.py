"""
Stage 4: Arc length + curvature computation per cubic Bezier.
- Arc length: 5-point Gauss-Legendre quadrature on |B'(t)|
- Curvature:  κ(t) = |B'×B''| / |B'|³  sampled at N_KAPPA points
Output: list of CurveMetrics(curve, path_length_mm, kappa_max, kappa_samples)
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from stage1 import CubicBezier
from stage2 import load_svg_mm
from stage3 import enforce_c1
from config import default as _config_default
from collections import namedtuple

CurveMetrics = namedtuple("CurveMetrics", [
    "curve",            # original CubicBezier
    "path_length_mm",   # arc length in mm
    "kappa_max",        # maximum curvature (mm⁻¹)
    "kappa_samples",    # list of (t, kappa) at N_KAPPA evenly-spaced t values
])

# ── Gauss-Legendre 5-point weights and abscissae on [0,1] ────────────────────
# Standard nodes/weights on [-1,1] mapped to [0,1]: t = (x+1)/2, w' = w/2

_GL5_NODES = [
    0.5 * (1 + x) for x in [
        -0.9061798459,
        -0.5384693101,
         0.0,
         0.5384693101,
         0.9061798459,
    ]
]
_GL5_WEIGHTS = [
    0.5 * w for w in [
        0.2369268851,
        0.4786286705,
        0.5688888889,
        0.4786286705,
        0.2369268851,
    ]
]

# ── Bezier derivatives ────────────────────────────────────────────────────────

def _bezier_point(c, t):
    mt = 1 - t
    return (
        mt**3*c.p0[0] + 3*mt**2*t*c.p1[0] + 3*mt*t**2*c.p2[0] + t**3*c.p3[0],
        mt**3*c.p0[1] + 3*mt**2*t*c.p1[1] + 3*mt*t**2*c.p2[1] + t**3*c.p3[1],
    )

def _bezier_deriv1(c, t):
    """B'(t) — first derivative."""
    mt = 1 - t
    return (
        3*(mt**2*(c.p1[0]-c.p0[0]) + 2*mt*t*(c.p2[0]-c.p1[0]) + t**2*(c.p3[0]-c.p2[0])),
        3*(mt**2*(c.p1[1]-c.p0[1]) + 2*mt*t*(c.p2[1]-c.p1[1]) + t**2*(c.p3[1]-c.p2[1])),
    )

def _bezier_deriv2(c, t):
    """B''(t) — second derivative."""
    mt = 1 - t
    return (
        6*(mt*(c.p2[0]-2*c.p1[0]+c.p0[0]) + t*(c.p3[0]-2*c.p2[0]+c.p1[0])),
        6*(mt*(c.p2[1]-2*c.p1[1]+c.p0[1]) + t*(c.p3[1]-2*c.p2[1]+c.p1[1])),
    )

# ── arc length ────────────────────────────────────────────────────────────────

def arc_length(c):
    """5-point Gauss-Legendre quadrature of |B'(t)| over [0,1]."""
    total = 0.0
    for t, w in zip(_GL5_NODES, _GL5_WEIGHTS):
        d = _bezier_deriv1(c, t)
        total += w * math.sqrt(d[0]**2 + d[1]**2)
    return total

# ── curvature ─────────────────────────────────────────────────────────────────

N_KAPPA = _config_default().quality.n_kappa  # curvature scan sample count (single-sourced)

def curvature(c, t):
    """κ(t) = |B'×B''| / |B'|³  (2D cross product = scalar)."""
    d1 = _bezier_deriv1(c, t)
    d2 = _bezier_deriv2(c, t)
    cross = d1[0]*d2[1] - d1[1]*d2[0]          # |B'×B''|
    speed = math.sqrt(d1[0]**2 + d1[1]**2)
    if speed < 1e-10:
        return 0.0
    return abs(cross) / (speed**3)

def kappa_samples(c, n=N_KAPPA):
    return [(t, curvature(c, t)) for t in [i/(n-1) for i in range(n)]]

# ── main stage ────────────────────────────────────────────────────────────────

def _kappa_max_moving(c, samples):
    """
    Curve's max curvature, IGNORING near-stationary samples.

    κ = |B'×B''|/|B'|³ blows up where |B'| -> 0 (a near-cusp / degenerate
    endpoint with nearly coincident control points). Such a spike spans almost
    zero arc length, but a plain max() lets it dominate kappa_max and drag the
    whole curve's planned speed to the floor (stage5 caps by kappa_max). Exclude
    samples whose speed is far below the curve's mean; the genuine tight features
    (tight AND moving) are kept, and stage6's per-axis interval limiter still
    slows the A axis locally at the artifact point.
    """
    speeds = [math.hypot(*_bezier_deriv1(c, t)) for t, _ in samples]
    mean_speed = sum(speeds) / len(speeds) if speeds else 0.0
    thresh = 0.1 * mean_speed
    moving = [k for (t, k), sp in zip(samples, speeds) if sp >= thresh]
    return max(moving) if moving else max((k for _, k in samples), default=0.0)

def compute_metrics(curves):
    """Returns list of CurveMetrics, one per input curve."""
    result = []
    for c in curves:
        samples = kappa_samples(c)
        result.append(CurveMetrics(
            curve=c,
            path_length_mm=arc_length(c),
            kappa_max=_kappa_max_moving(c, samples),
            kappa_samples=samples,
        ))
    return result

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    _q = _config_default().quality
    parser = argparse.ArgumentParser(description="Stage 4: arc length + curvature")
    parser.add_argument("svg", help="Path to SVG file")
    parser.add_argument("--angle-tol", type=float, default=_q.angle_tol)
    parser.add_argument("--gap-tol",   type=float, default=_q.gap_tol)
    args = parser.parse_args()

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, _  = enforce_c1(curves_mm, args.angle_tol, args.gap_tol)
    metrics      = compute_metrics(repaired)

    total = sum(m.path_length_mm for m in metrics)
    print(f"Curves: {len(metrics)}   Total path length: {total:.3f} mm\n")
    for i, m in enumerate(metrics):
        print(f"  [{i:2d}]  length={m.path_length_mm:8.3f} mm   kappa_max={m.kappa_max:.6f} 1/mm")
