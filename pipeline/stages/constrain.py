"""
Constrain stage (redesign stage 5): assign each Sample a LOCAL velocity ceiling.

Pure, per-sample, no propagation — "how fast could the tool ever go right here?"
The forward/backward feasibility sweeps that turn these ceilings into a reachable
profile are the Plan stage (plan_lookahead.py). Splitting the local cap from the
global sweep is the seam that the tile-era stage5 fused.

Ceiling at sample i = min of:
  feed_max                         programmed cruise limit
  sqrt(a_lat / kappa_i)            centripetal (XY radial accel) — LOCAL kappa
  rad(a_rate) / kappa_i            A-axis slew: a tangential tool rotates at
                                   dθ/dt = kappa·v, so cap v to the A slew ceiling
  junction-deviation cap           at a curve-boundary tangent jump below the
                                   corner threshold (GRBL-style cornering)
  0                                at a curve-boundary jump >= corner threshold
                                   (a lift-pivot corner — the tool must stop)

Using LOCAL kappa per sample is the whole point: a degenerate curvature spike caps
ONE sample, not a whole curve. The tile-era kappa_max conservatism (and its
_kappa_max_moving patch) cannot arise here.
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from config import default as _config_default
from sample import Sample, CURVE_BOUNDARY


def _angle_delta(a, b):
    """Shortest signed rotation a->b in degrees, range +/-180."""
    d = b - a
    while d > 180:  d -= 360
    while d < -180: d += 360
    return d


def _junction_cap(turn_deg, a_lat, deviation, feed_max):
    """
    GRBL junction-deviation cornering speed for a tangent turn of turn_deg across
    a near-zero-length boundary. Models the corner as a circular arc deviating
    from the exact vertex by at most `deviation` mm; returns the speed holding
    centripetal accel at a_lat on that arc. Straight -> feed_max; reversal -> 0.
    """
    half_cos = math.cos(math.radians(abs(turn_deg)) / 2.0)
    if half_cos >= 1.0 - 1e-9:
        return feed_max
    if half_cos <= 1e-9:
        return 0.0
    radius = deviation * half_cos / (1.0 - half_cos)
    return min(feed_max, math.sqrt(a_lat * radius))


def constrain(samples, feed_max, a_max, a_rate_deg_s=0.0,
              corner_stop_angle_deg=None, junction_deviation=None):
    """
    Set sample.v_ceiling in place for every sample; returns the list.

    feed_max  — programmed cruise ceiling (mm/s).
    a_max     — lateral acceleration for the centripetal cap (mm/s^2); also the
                accel the Plan stage ramps with, so they agree.
    a_rate_deg_s — tangential tool A-slew ceiling (deg/s); 0 disables the A cap
                (e.g. a pen).
    corner_stop_angle_deg — boundary tangent jump (deg) at/above which v_ceiling
                is forced to 0 so the tool can lift-pivot there. None disables
                forced stops (e.g. a pen corners via junction deviation only).
    junction_deviation — corner-rounding budget (mm); defaults to
                config.default().motion.junction_deviation.
    """
    if junction_deviation is None:
        junction_deviation = _config_default().motion.junction_deviation
    a_rate_rad = math.radians(a_rate_deg_s) if a_rate_deg_s > 0.0 else 0.0

    prev = None
    for s in samples:
        cap = feed_max
        if s.kappa > 1e-9:
            cap = min(cap, math.sqrt(a_max / s.kappa))
            if a_rate_rad > 0.0:
                cap = min(cap, a_rate_rad / s.kappa)

        # Tangent jump across a curve boundary (the corner signal). prev is the
        # previous curve's t=1 sample; this one is the next curve's t=0 (ds~0).
        if (s.flags & CURVE_BOUNDARY) and prev is not None:
            turn = _angle_delta(prev.theta, s.theta)
            if corner_stop_angle_deg is not None and abs(turn) >= corner_stop_angle_deg:
                cap = 0.0
            elif abs(turn) > 1e-6:
                cap = min(cap, _junction_cap(turn, a_max, junction_deviation, feed_max))

        s.v_ceiling = cap
        prev = s

    return samples


# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    from stage2 import load_svg_mm_subpaths
    from stage3 import enforce_c1
    from flatten import flatten

    cfg = _config_default()
    parser = argparse.ArgumentParser(description="Constrain stage: per-sample v ceiling")
    parser.add_argument("svg", help="Path to SVG file")
    parser.add_argument("--feed-max", type=float, default=cfg.motion.feed_max)
    parser.add_argument("--a-max",    type=float, default=cfg.motion.a_max)
    parser.add_argument("--a-rate",   type=float, default=cfg.machine.a.max_rate)
    parser.add_argument("--corner-stop", type=float, default=20.0)
    args = parser.parse_args()

    subpaths_mm, _ = load_svg_mm_subpaths(args.svg)
    repaired = [enforce_c1(sp)[0] for sp in subpaths_mm]
    samples  = flatten(repaired, quality=cfg.quality)
    constrain(samples, args.feed_max, args.a_max, a_rate_deg_s=args.a_rate,
              corner_stop_angle_deg=args.corner_stop)

    caps = [s.v_ceiling for s in samples]
    n_stop = sum(1 for c in caps if c == 0.0)
    print(f"Samples       : {len(samples)}")
    print(f"v_ceiling      : {min(caps):.2f} - {max(caps):.2f} mm/s")
    print(f"corner stops   : {n_stop}")
