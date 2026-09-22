#!/usr/bin/env python3
"""hall_phase.py — belt-phase-indexed correction, and how few laps buy it.

The A axis carries a periodic position error: the index is a fixed OUTPUT
angle, but the MOTOR step count at which it appears swings by about +-46 steps
(+-1.0 deg) on a period of 8.81 laps. That period is the belt loop aliasing --
the belt advances a non-integer fraction of a loop per output revolution, so
its phase creeps lap to lap instead of repeating.

Periodic and deterministic means correctable, which is the whole point: it is
by far the largest error left on the axis once the config scale factor is
fixed, and it is 10x the 0.09 deg homing floor.

THE AWKWARD PART, stated up front. Homing does not tell you the belt phase.
The index is one output angle and the belt only returns to the same phase every
8.81 revolutions, which is not an integer, so knowing the angle leaves you
ignorant of where in the belt cycle you are. The correction therefore needs a
phase measurement, and the question this file exists to answer is how expensive
that has to be. A full period is 9 laps, about 60 s at 400 us/step -- far too
slow to run at every startup.

But phase does not need a full period. With a 46-step amplitude against ~5
steps of per-lap measurement noise, a few consecutive index positions already
constrain it: their pattern of deviation from ideal spacing is distinctive.
Two laps leave a two-fold ambiguity, since a cosine is even about its peak.
Three should resolve it. This measures where that actually lands.

METHOD, and why it is not circular. The model -- period and amplitude -- is fit
on one set of captures and then applied to a DIFFERENT capture, which it has
never seen. Only the phase and the constant offset are estimated on the target,
from the first k laps alone, and the score is the prediction error over the laps
after those. Steps per revolution is held FIXED at its known value rather than
refitted, which matters more than it sounds: see solve_phase.

MEASURED, on a 31-lap forward capture the model had never seen, with the model
taken from the earlier 15-lap sweeps:

    k = 2, 3    correction is WORSE than no correction
    k = 4       0.17 deg     <- phase locks here, sharply
    k >= 5      0.15-0.19 deg, flat

Four laps, about 26 s at 400 us/step. The threshold at 4 reproduced exactly
between the 15-lap and 31-lap sets, which is what says it is a property of the
problem and not of one dataset. Below it the phase is genuinely unresolved --
a cosine is even about its peak, so short windows cannot tell which side they
are on, and a wrong-signed correction is worse than none.

RUNTIME SCHEME. Do not fit a phase once and extrapolate; the index passes once
per lap for free, so refresh it from a trailing window. Rolling, scored
causally on the 31-lap capture:

    trailing 5 laps   0.150 deg        trailing 8 laps   0.12-0.13 deg
    trailing 6 laps   0.137-0.147 deg

Horizon barely matters -- predicting 4 laps ahead scores the same as 1 -- so
the limit is phase-estimation noise from a short window, NOT drift over the
horizon. Lengthening the trailing window helps; predicting less often does not.

WHAT CAPS IT at ~0.13 deg rather than the 0.09 deg homing floor:

  * The period creeps. Within one continuous capture the single-sinusoid period
    moves 8.79 -> 9.03 -> 9.10. A belt meshing without slip cannot do that, so
    something is creeping. This is why long extrapolation degrades (0.26 deg
    across 27 laps) while short-horizon rolling does not.

  * There is a real second harmonic at P/2, amplitude ~5 steps. Adding it cuts
    held-out error about 30% (12.1 -> 8.4 steps). Worth having, not decisive.

DIRECTION MATTERS, and this is settled now rather than suspected. Over 31
forward and 29 reverse laps the amplitude is stable within each direction and
clearly different between them:

    forward   A 44.1 steps     reverse   A 29.7 steps     period ~9.00 both

The earlier 15-lap disagreement (32.3 vs 26.2) was small-sample noise. Steps per
revolution comes out 16497.77 forward and 16497.78 reverse -- it MUST be
direction-independent, so that agreement is a correctness check on the whole
chain. A purely geometric error would not care about direction either; that the
amplitude does says part of it is tension or lag, which fits a belt chosen to
be compliant.

So a correction needs one period and TWO amplitudes.
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hall_analyze as ha


def positions(path):
    """Index positions in motor steps, one per lap."""
    s, v, _ = ha.load(path, skip=1)
    b, d, spans, _r, _dr = ha.find_dips(v)
    return np.asarray([ha.est_mirror(np.arange(lo, hi, dtype=float), v[lo:hi], b)
                       for lo, hi in spans], float)


def design(k, P):
    return np.column_stack([np.ones_like(k), k,
                            np.cos(2 * np.pi * k / P), np.sin(2 * np.pi * k / P)])


def fit_model(pos, plo=4.0, phi=20.0, step=0.005):
    """Scan the period; return (P, amplitude, steps_per_rev)."""
    k = np.arange(len(pos), dtype=float)
    best = None
    for P in np.arange(plo, phi, step):
        M = design(k, P)
        c, *_ = np.linalg.lstsq(M, pos, rcond=None)
        r = float(np.sqrt(np.mean((pos - M @ c) ** 2)))
        if best is None or r < best[0]:
            best = (r, P, c)
    _, P, c = best
    return P, float(np.hypot(c[2], c[3])), float(c[1])


def solve_phase(pos_k, k_idx, P, A, slope):
    """Estimate belt phase from k index positions, with everything else known.

    At runtime the node already knows the period, the amplitude and steps per
    revolution -- those come from a calibration pass and do not change. The one
    thing homing cannot tell it is WHERE IN THE BELT CYCLE it is. So this
    solves for that alone, plus the constant offset, and nothing else.

    That distinction turns out to matter enormously. Letting a short window
    re-estimate the slope as well made 2-4 lap windows worse than no correction
    at all: a slope fit through 3 points, extrapolated across 12, is wild, and
    the wildness swamps the 46-step signal being corrected. Holding it fixed at
    the known 16497.9 removes a free parameter that short windows cannot afford.

    Phase is grid-searched rather than fitted as a*cos + b*sin, because the
    linear form cannot enforce a known amplitude -- it will happily shrink the
    sinusoid toward zero to fit noise, which is precisely how a 2-lap window
    fools itself into looking good. Here A is pinned and only the phase moves.
    """
    best = None
    for phi in np.linspace(0.0, 2 * np.pi, 721)[:-1]:
        wave = A * np.cos(2 * np.pi * k_idx / P + phi)
        off = float(np.mean(pos_k - slope * k_idx - wave))
        r = float(np.sum((pos_k - slope * k_idx - wave - off) ** 2))
        if best is None or r < best[0]:
            best = (r, phi, off)
    _, phi, off = best
    return phi, off, slope


def predict_phase(phi, off, slope, k_idx, P, A):
    return off + slope * k_idx + A * np.cos(2 * np.pi * k_idx / P + phi)


def predict(coef, k_idx, P):
    return design(k_idx, P) @ coef


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", nargs="+", required=True,
                    help="captures the period and amplitude are learned from")
    ap.add_argument("--target", required=True,
                    help="a DIFFERENT capture, used only to test")
    ap.add_argument("--max-laps", type=int, default=10,
                    help="largest phase-finding window to try")
    args = ap.parse_args()

    Ps, As = [], []
    for p in args.model:
        P, A, spr = fit_model(positions(p))
        Ps.append(P); As.append(A)
        print(f"  model {os.path.basename(p):34s} P {P:6.3f}  A {A:5.1f}"
              f"  steps/rev {spr:9.2f}")
    P, A = float(np.mean(Ps)), float(np.mean(As))
    print(f"\n  model used: period {P:.3f} laps, amplitude {A:.1f} steps"
          f"  (from {len(Ps)} capture(s))")

    pos = positions(args.target)
    n = len(pos)
    k = np.arange(n, dtype=float)
    Pt, At, sprt = fit_model(pos)
    print(f"  target {os.path.basename(args.target)}: {n} laps, "
          f"its own best fit P {Pt:6.3f}  A {At:5.1f}  steps/rev {sprt:9.2f}")
    print(f"      -> period agrees to {abs(Pt-P):.3f} laps, "
          f"amplitude to {abs(At-A):.1f} steps\n")

    spr = float(np.mean([fit_model(positions(p))[2] for p in args.model]))
    print(f"  steps/rev held fixed at {spr:.2f} (known; not refitted per window)")
    print("\n  phase from the first k laps, scored on every lap after them:")
    print("     k    laps scored    corrected rms    uncorrected    gain")
    deg = 360.0 / 16497.87
    for kk in range(2, min(args.max_laps, n - 2) + 1):
        phi, off, s = solve_phase(pos[:kk], k[:kk], P, A, spr)
        kte = k[kk:]
        err = pos[kk:] - predict_phase(phi, off, s, kte, P, A)
        off0 = float(np.mean(pos[:kk] - spr * k[:kk]))
        err0 = pos[kk:] - (off0 + spr * kte)
        rms = float(np.sqrt(np.mean(err ** 2)))
        rms0 = float(np.sqrt(np.mean(err0 ** 2)))
        print(f"    {kk:2d}    {n-kk:9d}    {rms:8.2f} steps   {rms0:8.2f}"
              f"     {rms0/rms:5.2f}x     ({rms*deg:.3f} deg vs {rms0*deg:.3f})")

    print("\n  (a two-lap window cannot resolve which side of the cosine peak")
    print("   it sits on; watch for the jump when that ambiguity breaks)")


if __name__ == "__main__":
    main()
