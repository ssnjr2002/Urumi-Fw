"""
Flatten stage (redesign stage 4): repaired Bezier subpaths -> flat list[Sample].

This is the representation drop the whole redesign turns on: after here the
pipeline no longer sees Bezier tiles, only an arc-length sample stream carrying
PER-SAMPLE local curvature. The tile-era conservatism (one cruise speed per
curve, capped by the curve's MAX curvature) is structurally impossible once the
unit is the sample. See PLAN_pipeline_redesign.md S1.

Sampling is driven by two geometry-only limits (velocity is unknown here — that
is the point; planning happens downstream):
  1. chord deviation:  dt <= sqrt(8 * chord_tol / |B''(t)|)   — finer where curved
  2. spacing cap:       dt <= ds_max / |B'(t)|                 — bounded everywhere
The spacing cap guarantees enough samples on long straight runs for the
look-ahead's accel/decel ramps to be smooth (premortem P3), where chord
deviation alone would emit only a handful.

Curve boundaries are kept as adjacent samples (prev curve t=1, next curve t=0)
with ds~0 between them: a smooth join is a harmless near-duplicate; a sharp
corner shows up as two samples with the SAME position but a large theta jump —
that jump is the corner signal the Constrain / Choreograph stages read.
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
from pipeline.stages.bezier import (bezier_point as _bezier_point,
                    bezier_deriv1 as _bezier_deriv1,
                    bezier_deriv2 as _bezier_deriv2, curvature)
from pipeline.stages.config import default as _config_default
from pipeline.stages.sample import Sample, PATH_START, PATH_END, CURVE_BOUNDARY


def _tangent_deg(c, t, fallback=0.0):
    """Tangent angle at t in degrees; fallback where the curve is stationary."""
    d1 = _bezier_deriv1(c, t)
    if d1[0] * d1[0] + d1[1] * d1[1] < 1e-20:
        return fallback
    return math.degrees(math.atan2(d1[1], d1[0]))


def _dt_at(c, t, chord_tol, ds_max, dtheta_max, dt_max):
    """
    Geometry-only adaptive step: min of three caps —
      chord deviation  (position error)         : dt <= sqrt(8*chord_tol/|B''|)
      spacing          (facet length)           : dt <= ds_max/|B'|
      tangent step     (angular turn per sample) : dt <= dtheta_max/(kappa*|B'|)
    The angular cap is what keeps a tangential knife smooth on curves: chord
    deviation alone allows a large tangent jog over a low-deviation chord.
    """
    dt = dt_max
    speed = math.hypot(*_bezier_deriv1(c, t))
    d2 = _bezier_deriv2(c, t)
    mag2 = d2[0] * d2[0] + d2[1] * d2[1]
    if mag2 > 1e-20:
        dt = min(dt, math.sqrt(8.0 * chord_tol / math.sqrt(mag2)))
    if speed > 1e-12:
        dt = min(dt, ds_max / speed)
        k = curvature(c, t)
        if k > 1e-9:
            dt = min(dt, math.radians(dtheta_max) / (k * speed))
    return dt


def _ts_for_curve(c, chord_tol, ds_max, dtheta_max, dt_max, dt_min):
    """Parameter values [0..1] at which to sample one curve (both ends inclusive)."""
    ts = [0.0]
    t = 0.0
    while t < 1.0:
        dt = max(dt_min, _dt_at(c, t, chord_tol, ds_max, dtheta_max, dt_max))
        t = min(t + dt, 1.0)
        ts.append(t)
    return ts


def flatten(subpaths, quality=None):
    """
    Flatten repaired Bezier subpaths into one flat list[Sample].

    subpaths — list[list[CubicBezier]] (mm, machine frame), as stage3 emits per
               subpath. Each inner list is one continuous pen-down stroke.
    quality  — QualityConfig (chord_tol, ds_max, dt_max, dt_min). Defaults to
               config.default().quality.

    Returns a single list[Sample]; PATH_START / PATH_END bracket each subpath and
    CURVE_BOUNDARY marks intra-subpath curve joins. ds on each sample is the chord
    to the next sample (0.0 on the final sample of each subpath).
    """
    q = quality if quality is not None else _config_default().quality
    out = []

    for subpath in subpaths:
        if not subpath:
            continue
        sub_start = len(out)
        prev_theta = _tangent_deg(subpath[0], 0.0)

        for ci, c in enumerate(subpath):
            ts = _ts_for_curve(c, q.chord_tol, q.ds_max, q.dtheta_max,
                               q.dt_max, q.dt_min)
            for k, t in enumerate(ts):
                # Skip a curve's t=0 when it coincides with the previous curve's
                # t=1 AND the join is smooth — otherwise keep it (corners need the
                # second tangent). Cheap rule: always keep, flag boundary; ds~0
                # duplicates are inert to the look-ahead. Simplicity over a merge.
                px, py = _bezier_point(c, t)
                theta = _tangent_deg(c, t, fallback=prev_theta)
                kappa = curvature(c, t)
                flags = 0
                if ci > 0 and k == 0:
                    flags |= CURVE_BOUNDARY
                out.append(Sample(x=px, y=py, theta=theta, kappa=kappa,
                                  ds=0.0, flags=flags))
                prev_theta = theta

        # mark subpath ends
        out[sub_start].flags |= PATH_START
        out[-1].flags |= PATH_END

        # fill ds = chord to next sample, within this subpath only
        for i in range(sub_start, len(out) - 1):
            out[i].ds = math.hypot(out[i + 1].x - out[i].x,
                                   out[i + 1].y - out[i].y)
        out[-1].ds = 0.0

    return out


# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    from host.production.normalise import load_svg_mm_subpaths
    from host.production.repair import enforce_c1

    cfg = _config_default()
    parser = argparse.ArgumentParser(description="Flatten stage: SVG -> Sample stream")
    parser.add_argument("svg", help="Path to SVG file")
    parser.add_argument("--chord-tol", type=float, default=cfg.quality.chord_tol)
    parser.add_argument("--ds-max",    type=float, default=cfg.quality.ds_max)
    parser.add_argument("--angle-tol", type=float, default=cfg.quality.angle_tol)
    parser.add_argument("--gap-tol",   type=float, default=cfg.quality.gap_tol)
    args = parser.parse_args()

    subpaths_mm, _ = load_svg_mm_subpaths(args.svg)
    repaired = [enforce_c1(sp, args.angle_tol, args.gap_tol)[0] for sp in subpaths_mm]

    samples = flatten(repaired, quality=cfg.quality)

    total_len = sum(s.ds for s in samples)
    n_start = sum(1 for s in samples if s.flags & PATH_START)
    print(f"Subpaths      : {n_start}")
    print(f"Samples       : {len(samples)}")
    print(f"Total length  : {total_len:.3f} mm")
    print(f"kappa range   : {min(s.kappa for s in samples):.5f} - "
          f"{max(s.kappa for s in samples):.5f} 1/mm")
