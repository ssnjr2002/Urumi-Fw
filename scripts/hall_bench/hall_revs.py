#!/usr/bin/env python3
"""hall_revs.py — how many revolutions does a given measurement actually need?

Throwaway bench tool. Answers two questions the bake-off does not:

  1. How does each measurement's uncertainty shrink with lap count? Averaging
     independent noise buys 1/sqrt(N); a systematic error buys nothing. Running
     the same reduction on 2,3,...,N laps shows which regime we are in, and so
     where more revolutions stop paying.

  2. Is the lap-to-lap dip spacing random, or structured? If the belt is the
     dominant error it is a function of belt phase, not lap number, so the
     spacing sequence should show autocorrelation rather than looking white.
     Random -> more laps help. Structured -> a correction table helps instead.
"""
import argparse
import itertools

import numpy as np

import hall_analyze as ha


def positions(path, skip, est):
    s, v, meta = ha.load(path, skip=skip)
    baseline, depth, spans, _raw, _dropped = ha.find_dips(v)
    out = []
    for lo, hi in spans:
        x = np.arange(lo, hi, dtype=float)
        c = est(x, v[lo:hi], baseline)
        if np.isfinite(c):
            out.append(c)
    return np.asarray(out, float)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--skip", type=int, default=1)
    ap.add_argument("--estimator", default="est_mirror")
    args = ap.parse_args()

    est = getattr(ha, args.estimator)

    for path in args.csv:
        pos = positions(path, args.skip, est)
        sp = np.diff(pos)
        spr = sp.mean()

        print(f"\n=== {path}  ({args.estimator}, {len(pos)} dips) ===")
        print(f"  steps/rev  {spr:9.2f}   steps/deg {spr / 360:7.4f}   "
              f"deg/step {360 / spr:.6f}")
        print(f"  spacing    sd {sp.std(ddof=1):6.2f} steps "
              f"({sp.std(ddof=1) * 360 / spr:.4f} deg)  "
              f"range {sp.min():.0f}-{sp.max():.0f}")

        # --- 1. does more laps help? sub-sample the run at each lap count ---
        # For each N, take every contiguous window of N+1 dips, estimate
        # steps/rev from its endpoints (the widest baseline available, which is
        # how you would actually use it), and report the spread of estimates.
        print("\n  steps/rev estimated over an N-lap baseline:")
        print("     N laps   spread(sd)    as deg      windows")
        for n in range(1, len(pos)):
            ests = [(pos[i + n] - pos[i]) / n for i in range(len(pos) - n)]
            if len(ests) < 2:
                break
            sd = float(np.std(ests, ddof=1))
            print(f"  {n:9d}   {sd:9.2f}   {sd * 360 / spr:8.4f}   {len(ests):9d}")

        # --- 2. is the spacing structured or white? ---
        if len(sp) > 4:
            d = sp - sp.mean()
            ac = [float(np.corrcoef(d[:-k], d[k:])[0, 1]) for k in range(1, 5)]
            print("\n  spacing autocorrelation at lag 1..4: " +
                  "  ".join(f"{a:+.2f}" for a in ac))
            print("  (near zero = white, more laps average it down; "
                  "large = structured, a correction table helps)")


if __name__ == "__main__":
    main()
