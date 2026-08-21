#!/usr/bin/env python3
"""hall_bidir.py — compare a forward and a reverse sweep of the same axis.

Throwaway bench tool, paired with hall_capture.py / hall_analyze.py.

Reversing the sweep flips the SPATIAL axis but leaves any TIME lag pointing the
same way in time, so the two separate cleanly:

    mean of the forward and reverse index phase  = true spatial centre
    half their difference                        = lag + backlash + wind-up

This only works if the reverse run retraced the forward run's path, which is
what happens when the two captures are run back to back with the same step
count and pre-roll and nothing moves the axis in between. With s0 as the motor
position at the start of the forward pre-roll:

    forward sample i  ->  physical  p = preroll + i
    reverse sample j  ->  physical  p = steps - j

The forward run ends at preroll+steps; the reverse run's own pre-roll walks
2000 steps back from there, to preroll+steps-preroll = steps, and counts down
from there. So the pre-roll cancels out of the reverse expression rather than
appearing in it — get this wrong and the whole pre-roll shows up as a fake
2000-step lag.

Belt error is a function of physical angle, so it is common to both runs and
cancels in the difference — which is the whole reason to compare in physical
coordinates rather than per-run lap number.

  python hall_bidir.py hall_i1000_d0_n247500.csv hall_i1000_d1_n247500.csv
"""
import argparse

import numpy as np

import hall_analyze as ha


def index_phase(path, reverse, steps, preroll, skip, est):
    """Index positions of one capture, in physical motor steps, plus the
    per-revolution phase of each."""
    s, v, meta = ha.load(path, skip=skip)
    baseline, depth, spans, _raw, _dropped = ha.find_dips(v)

    pos = []
    for lo, hi in spans:
        x = np.arange(lo, hi, dtype=float)
        c = est(x, v[lo:hi], baseline)
        if np.isfinite(c):
            pos.append(c)
    pos = np.asarray(pos, float)

    phys = (steps - pos) if reverse else (preroll + pos)
    return np.sort(phys), meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("forward")
    ap.add_argument("reverse")
    ap.add_argument("--steps", type=int, default=247500)
    ap.add_argument("--preroll", type=int, default=2000)
    ap.add_argument("--skip", type=int, default=1)
    ap.add_argument("--both-forward", action="store_true",
                    help="second file is another FORWARD sweep, not a reverse "
                         "one. Use this to compare two speeds: the difference "
                         "is then speed-dependent bias rather than lag.")
    ap.add_argument("--estimator", default="est_mirror",
                    help="any estimator name from hall_analyze")
    args = ap.parse_args()

    est = getattr(ha, args.estimator)

    fwd, mf = index_phase(args.forward, False, args.steps, args.preroll,
                          args.skip, est)
    rev, mr = index_phase(args.reverse, not args.both_forward, args.steps,
                          args.preroll, args.skip, est)

    # Steps per revolution from the forward run's own dip spacing; used only to
    # fold physical position into a phase, so a small error here is harmless.
    spr = float(np.mean(np.diff(fwd)))

    print(f"\n=== bidirectional: {args.estimator} ===")
    print(f"  forward  {args.forward}  ({len(fwd)} dips)")
    print(f"  reverse  {args.reverse}  ({len(rev)} dips)")
    print(f"  steps/rev (forward spacing): {spr:.1f}")

    # Pair each forward dip with the nearest reverse dip in physical position.
    # They should land within a fraction of a revolution of each other.
    pairs = []
    for p in fwd:
        j = int(np.argmin(np.abs(rev - p)))
        d = rev[j] - p
        if abs(d) < spr / 2:
            pairs.append((p, rev[j], d))

    if not pairs:
        raise SystemExit("no forward/reverse dips paired — were the two runs "
                         "taken back to back with matching --steps/--preroll?")

    d = np.array([p[2] for p in pairs])
    deg = 360.0 / spr

    print(f"\n  paired dips: {len(pairs)}")
    print(f"  reverse - forward:  mean {d.mean():+8.2f} steps "
          f"({d.mean() * deg:+.3f} deg)   sd {d.std(ddof=1):.2f}")
    print(f"  half-difference  =  lag + backlash + wind-up: "
          f"{d.mean() / 2:+.2f} steps ({d.mean() / 2 * deg:+.3f} deg)")
    print("\n  per-pair (physical steps):")
    print("      forward      reverse      rev-fwd")
    for a, b, dd in pairs:
        print(f"  {a:11.1f}  {b:11.1f}  {dd:+11.2f}")


if __name__ == "__main__":
    main()
