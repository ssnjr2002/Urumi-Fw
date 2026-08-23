#!/usr/bin/env python3
"""hall_backlash.py — measure backlash directly, on the dip flank.

Throwaway bench tool. Needs no firmware beyond what src/scratch/hall_capture.cpp
already has: h, m, d, r.

THE IDEA. Every other measurement in this directory treats the Hall dip as an
INDEX -- a once-per-revolution fiducial whose centre you estimate. But the dip's
FLANK is something else entirely: a region where the field changes ~4.6 counts
per step, monotonically, over roughly +-450 steps either side of centre. Against
23 counts of noise that is a position sensor with a few steps of single-sample
resolution, better with averaging, reading the OUTPUT shaft past the belt.

So for about 20 degrees of the 360, the axis has genuine load-side position
feedback. Not enough to home with, but plenty for a short-range RELATIVE
measurement -- which is exactly what backlash is.

WHY NOT MEASURE IT THE OTHER WAY. Comparing forward and reverse index positions
across whole revolutions gave 9.7 steps with a standard error near 5, because
each lap carries the belt error too. Here the whole measurement happens inside a
few hundred steps at one belt phase, so the belt contributes nothing: it cannot
move appreciably over the span being measured.

METHOD. Park on the steep flank having arrived moving FORWARD, so forward lost
motion is already taken up. Then:

    1. step forward, capture      -> the reference slope, counts per step
    2. reverse, capture           -> FLAT while lost motion is taken up,
                                     then the slope resumes
    3. forward again, capture     -> flat again, the other direction

The length of each flat region is the lost motion. Read off as the intersection
of the flat level with the resumed slope extrapolated back -- a two-line knee,
which is far more robust to noise than trying to spot where the curve "starts
moving".

  python hall_backlash.py --port COM15 --trials 4
"""
import argparse
import statistics
import sys

import numpy as np

from hall_home import Node, parse_kv

FLANK_OFFSET = 250      # steps before the dip centre: peak slope sits here
APPROACH     = 400      # run-up so the parking move ends going forward


def capture(node, interval, steps):
    """Run `r` and return the ADC samples as an array."""
    lines = node.cmd(f"r {interval} {steps} 0", terminator="# END",
                     timeout=60 + steps * interval / 1e6 * 3)
    out = []
    for t in lines:
        if t.startswith("#"):
            continue
        _, _, b = t.partition(",")
        try:
            out.append(int(b))
        except ValueError:
            pass
    return np.asarray(out, float)


def knee(v, fit_lo=12, fit_hi=55, flat_n=4):
    """Where does the trace stop being flat and start following the slope?

    Fits the resumed slope, fits the flat level, and intersects them. Both fits
    average away noise, so this is far tighter than any single-sample threshold
    test on a signal whose slope is only ~4 counts/step against 23 of noise.

    THE FIT WINDOW MATTERS, and getting it wrong is not subtle. The flank is the
    side of a dip: its local slope runs from 4.7 counts/step at 250 steps out to
    2.9 at 100 steps out. Fitting a straight line across 150 steps of that and
    extrapolating back to the head produced lost-motion values of 24 steps in
    one direction and NEGATIVE 14 in the other -- the negative being the tell,
    since lost motion cannot be below zero. Keep the fit inside a span short
    enough that the curvature does not matter.

    Returns (steps_of_lost_motion, slope_counts_per_step) or (nan, nan).
    """
    n = len(v)
    hi = min(fit_hi, n)
    if hi - fit_lo < 10:
        return float("nan"), float("nan")

    t = np.arange(fit_lo, hi, dtype=float)
    m, c = np.polyfit(t, v[fit_lo:hi], 1)
    if abs(m) < 1e-6:
        return float("nan"), float("nan")

    flat = float(np.mean(v[:flat_n]))
    x = (flat - c) / m                      # where the slope line hits the flat level
    return x, m


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--interval", type=int, default=1000)
    ap.add_argument("--steps", type=int, default=70,
                    help="steps captured per leg; needs to comfortably exceed "
                         "the lost motion so the resumed slope is well fitted")
    ap.add_argument("--trials", type=int, default=4)
    ap.add_argument("--budget", type=int, default=21447)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    node = Node(args.port, args.baud, args.verbose)
    try:
        node.cmd("e 1", terminator="# en")
        node.cmd("d 0", terminator="# dir")

        print("\n=== backlash on the dip flank ===")
        print(f"    {args.steps} steps per leg at {args.interval} us/step, "
              f"parking {FLANK_OFFSET} steps before dip centre\n")
        print("    trial   slope c/step    rev lost    fwd lost")

        rev_all, fwd_all, slopes = [], [], []
        for k in range(args.trials):
            # Home to find the dip, then park on its flank having arrived
            # moving forward, so forward lost motion is already taken up.
            lines = node.cmd(f"h {args.interval} {args.budget} 0", terminator="# HOME")
            hit = [t for t in lines if t.startswith("# HOME")]
            if not hit or "found=1" not in hit[0]:
                print(f"    {k:5d}   homing failed")
                continue
            d = parse_kv(hit[0])
            target = d["centre"] - FLANK_OFFSET

            node.cmd(f"m {int(target - APPROACH - d['pos'])} {args.interval}",
                     terminator="# MOVE")
            node.cmd(f"m {APPROACH} {args.interval}", terminator="# MOVE")

            node.cmd("d 0", terminator="# dir")
            ref = capture(node, args.interval, args.steps)      # reference slope

            node.cmd("d 1", terminator="# dir")
            rev = capture(node, args.interval, args.steps)      # reversal 1

            node.cmd("d 0", terminator="# dir")
            fwd = capture(node, args.interval, args.steps)      # reversal 2

            _, m_ref = knee(ref)
            x_rev, _ = knee(rev)
            x_fwd, _ = knee(fwd)

            slope = float(np.polyfit(np.arange(len(ref), dtype=float), ref, 1)[0])
            slopes.append(slope)
            if np.isfinite(x_rev):
                rev_all.append(x_rev)
            if np.isfinite(x_fwd):
                fwd_all.append(x_fwd)
            print(f"    {k:5d}   {slope:12.2f}   {x_rev:9.1f}   {x_fwd:9.1f}")

        deg = 360.0 / 16498
        print()
        for name, vals in (("reverse", rev_all), ("forward", fwd_all)):
            if len(vals) >= 2:
                mu, sd = statistics.mean(vals), statistics.stdev(vals)
                print(f"    {name:8s} lost motion  {mu:6.2f} steps "
                      f"({mu * deg:.4f} deg)   sd {sd:.2f}  "
                      f"sem {sd / len(vals) ** 0.5:.2f}")
            elif vals:
                print(f"    {name:8s} lost motion  {vals[0]:6.2f} steps "
                      f"({vals[0] * deg:.4f} deg)   (n=1)")

        both = rev_all + fwd_all
        if len(both) >= 2:
            mu = statistics.mean(both)
            print(f"\n    combined  {mu:6.2f} steps ({mu * deg:.4f} deg), "
                  f"n={len(both)}")
            print(f"    lap-based estimate for comparison: 9.7 steps "
                  f"(0.21 deg), sem ~5")
        if slopes:
            print(f"    flank slope {statistics.mean(slopes):.2f} counts/step "
                  f"(expected ~4.6 at this offset)")
    finally:
        node.close()


if __name__ == "__main__":
    main()
