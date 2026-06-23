"""
bezier.py — cubic Bezier geometry primitives shared across the pipeline.

Point, first/second derivative, arc length (5-point Gauss-Legendre), and
curvature. These were originally in stage4; they outlived the tile-era metrics
that surrounded them (compute_metrics / kappa_max), so they live here as the
neutral geometry layer the Flatten stage builds on.
"""

import math

# ── Gauss-Legendre 5-point weights and abscissae on [0,1] ─────────────────────
# Standard nodes/weights on [-1,1] mapped to [0,1]: t = (x+1)/2, w' = w/2

_GL5_NODES = [
    0.5 * (1 + x) for x in [
        -0.9061798459, -0.5384693101, 0.0, 0.5384693101, 0.9061798459,
    ]
]
_GL5_WEIGHTS = [
    0.5 * w for w in [
        0.2369268851, 0.4786286705, 0.5688888889, 0.4786286705, 0.2369268851,
    ]
]


def bezier_point(c, t):
    mt = 1 - t
    return (
        mt**3*c.p0[0] + 3*mt**2*t*c.p1[0] + 3*mt*t**2*c.p2[0] + t**3*c.p3[0],
        mt**3*c.p0[1] + 3*mt**2*t*c.p1[1] + 3*mt*t**2*c.p2[1] + t**3*c.p3[1],
    )


def bezier_deriv1(c, t):
    """B'(t) — first derivative."""
    mt = 1 - t
    return (
        3*(mt**2*(c.p1[0]-c.p0[0]) + 2*mt*t*(c.p2[0]-c.p1[0]) + t**2*(c.p3[0]-c.p2[0])),
        3*(mt**2*(c.p1[1]-c.p0[1]) + 2*mt*t*(c.p2[1]-c.p1[1]) + t**2*(c.p3[1]-c.p2[1])),
    )


def bezier_deriv2(c, t):
    """B''(t) — second derivative."""
    mt = 1 - t
    return (
        6*(mt*(c.p2[0]-2*c.p1[0]+c.p0[0]) + t*(c.p3[0]-2*c.p2[0]+c.p1[0])),
        6*(mt*(c.p2[1]-2*c.p1[1]+c.p0[1]) + t*(c.p3[1]-2*c.p2[1]+c.p1[1])),
    )


def arc_length(c):
    """5-point Gauss-Legendre quadrature of |B'(t)| over [0,1]."""
    total = 0.0
    for t, w in zip(_GL5_NODES, _GL5_WEIGHTS):
        d = bezier_deriv1(c, t)
        total += w * math.sqrt(d[0]**2 + d[1]**2)
    return total


def curvature(c, t):
    """κ(t) = |B'×B''| / |B'|³  (2D cross product = scalar)."""
    d1 = bezier_deriv1(c, t)
    d2 = bezier_deriv2(c, t)
    cross = d1[0]*d2[1] - d1[1]*d2[0]
    speed = math.sqrt(d1[0]**2 + d1[1]**2)
    if speed < 1e-10:
        return 0.0
    return abs(cross) / (speed**3)
