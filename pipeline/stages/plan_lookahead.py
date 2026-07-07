"""
Plan stage (redesign stage 6): the look-ahead feedrate planner.

Turns each sample's LOCAL v_ceiling (from Constrain) into a globally reachable
speed v[i] via two sweeps over the sample stream of a subpath:

  backward (decel feasibility), last -> first:
      v[i] = min(v_ceiling[i], sqrt(v[i+1]^2 + 2*a*ds[i]))
  forward (accel feasibility), first -> last:
      v[i] = min(v[i],         sqrt(v[i-1]^2 + 2*a*ds[i-1]))

After both, every sample's speed is simultaneously reachable-from-behind and
stoppable-ahead, so the profile is acceleration-continuous by construction — the
tile-era accel-continuity residual at curve junctions cannot occur, because
junctions are no longer planning boundaries (a sample mid-stream is identical to
one at a curve seam).

Boundary conditions: PATH_START and PATH_END force v = 0 (the tool starts/stops
at rest around the pen-up jog between subpaths). Corner-stop samples already carry
v_ceiling = 0 from Constrain; the zero-length gap on either side propagates the
stop to both neighbours, exactly the lift-pivot precondition.

PER-AXIS acceleration: the accel over a segment is the tool-path acceleration that
keeps every axis within its own limit. For unit travel direction (ux, uy),
a_seg = min(x.accel/|ux|, y.accel/|uy|) — on a diagonal each axis stays capped
while the tool accelerates faster than a single scalar would allow. Axes with
accel = 0 are treated as unlimited; if none constrain, the scalar a_max is used
(square-machine parity). A-axis angular accel is NOT projected here — the A-slew
ceiling in Constrain already bounds A velocity; A accel coupling is deferred.
"""

import math
import argparse
import sys, os
sys.path.insert(0, os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..")))
from pipeline.config import default as _config_default
from pipeline.stages.sample import Sample, PATH_START, PATH_END


def _subpath_ranges(samples):
    """Yield (start, end) inclusive index ranges, one per PATH_START..PATH_END."""
    start = None
    for i, s in enumerate(samples):
        if s.flags & PATH_START:
            start = i
        if s.flags & PATH_END:
            if start is None:
                start = i
            yield (start, i)
            start = None


def _seg_accel(s0, s1, machine, a_max):
    """
    Tool-path accel over the segment s0->s1 honouring per-axis accel limits.

    X and Y: the tool accel projects onto each axis as a*|u_axis|, so to keep each
    within its own limit, a <= min(x.accel/|ux|, y.accel/|uy|).

    A (tangential tracking): the tool speeding up while curved drives A angular
    accel α = κ·a_tan, so to keep A within its accel ceiling, a_tan <= rad(a.accel)/κ.
    This is the κ·a_tangential term that pairs with the curvature-gradient term
    Constrain handles as a velocity ceiling — together they bound A's total
    angular acceleration. a.accel == 0 (unset) skips it.
    """
    dx = s1.x - s0.x
    dy = s1.y - s0.y
    d = math.hypot(dx, dy)
    if d < 1e-12:
        return a_max
    ux, uy = abs(dx) / d, abs(dy) / d
    cands = []
    if machine.x.accel > 0 and ux > 1e-9:
        cands.append(machine.x.accel / ux)
    if machine.y.accel > 0 and uy > 1e-9:
        cands.append(machine.y.accel / uy)
    if machine.a.accel > 0:
        kap = max(s0.kappa, s1.kappa)
        if kap > 1e-9:
            cands.append(math.radians(machine.a.accel) / kap)
    return min(cands) if cands else a_max


def plan(samples, machine, a_max=None):
    """
    Resolve sample.v in place via the backward+forward look-ahead. Returns the
    list. a_max defaults to config.default().machine.x.accel (the XY-plane accel
    fallback; per-axis accels in `machine` override it per segment).
    """
    if a_max is None:
        a_max = _config_default().machine.x.accel

    for lo, hi in _subpath_ranges(samples):
        # init from ceilings; pin the endpoints to rest
        for i in range(lo, hi + 1):
            samples[i].v = samples[i].v_ceiling
        samples[lo].v = 0.0
        samples[hi].v = 0.0

        # precompute per-segment accel (segment i links sample i and i+1)
        a_seg = [0.0] * hi
        for i in range(lo, hi):
            a_seg[i] = _seg_accel(samples[i], samples[i + 1], machine, a_max)

        # backward: ensure we can brake to each downstream speed
        for i in range(hi - 1, lo - 1, -1):
            ds = samples[i].ds
            reachable = math.sqrt(samples[i + 1].v * samples[i + 1].v + 2.0 * a_seg[i] * ds)
            if reachable < samples[i].v:
                samples[i].v = reachable

        # forward: ensure we can accelerate up to each speed
        for i in range(lo + 1, hi + 1):
            ds = samples[i - 1].ds
            reachable = math.sqrt(samples[i - 1].v * samples[i - 1].v + 2.0 * a_seg[i - 1] * ds)
            if reachable < samples[i].v:
                samples[i].v = reachable

        # endpoints stay pinned (forward pass may have lifted hi off 0)
        samples[hi].v = 0.0

    return samples


# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    from host.production.normalise import load_svg_mm_subpaths
    from host.production.repair import enforce_c1
    from pipeline.stages.flatten import flatten
    from pipeline.stages.constrain import constrain
    from pipeline.config import PEN

    cfg = _config_default()
    parser = argparse.ArgumentParser(description="Plan stage: look-ahead feedrate")
    parser.add_argument("svg", help="Path to SVG file")
    parser.add_argument("--feed-max", type=float, default=PEN.feed_max)
    parser.add_argument("--a-max",    type=float, default=cfg.machine.x.accel)
    parser.add_argument("--a-rate",   type=float, default=cfg.machine.a.max_rate)
    parser.add_argument("--corner-stop", type=float, default=20.0)
    args = parser.parse_args()

    subpaths_mm, _ = load_svg_mm_subpaths(args.svg)
    repaired = [enforce_c1(sp)[0] for sp in subpaths_mm]
    samples  = flatten(repaired, quality=cfg.quality)
    constrain(samples, args.feed_max, args.a_max, a_rate_deg_s=args.a_rate,
              corner_stop_angle_deg=args.corner_stop)
    plan(samples, cfg.machine, a_max=args.a_max)

    # estimate job time: sum ds / mean adjacent speed
    t = 0.0
    for i in range(len(samples) - 1):
        ds = samples[i].ds
        vbar = max(0.5, 0.5 * (samples[i].v + samples[i + 1].v))
        t += ds / vbar
    vs = [s.v for s in samples]
    print(f"Samples       : {len(samples)}")
    print(f"v range        : {min(vs):.2f} - {max(vs):.2f} mm/s")
    print(f"Est. cut time  : {t:.1f} s")
