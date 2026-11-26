#!/usr/bin/env python3
"""hall_phase_node.py — does the node's fixed-point phase solve match the PC?

Two separate questions get confused if you only look at the end number:

  1. ARITHMETIC. Does the AVR's integer phase search, its 256-entry cosine
     table and its Q8 positions reproduce what float on a PC would do?
  2. PHYSICS. Does the belt model predict laps it has not seen?

This answers (1), by re-running the identical solve in float on the node's OWN
emitted residuals. Same input, two implementations, so any disagreement is
arithmetic and nothing else. Question (2) is what the node's own held-out rms
reports, and it is only trustworthy once (1) passes -- otherwise a good score
could be a fixed-point bug that happens to flatter the data.

This is the same discipline the mirror estimator got: the node and the PC were
made to chew the very same emitted window, and agreed to 0.04 steps.

  python hall_phase_node.py --port COM15 --laps 14 --win 6 --dir 0
"""
import argparse
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from hall_home import Node, parse_kv

BELT_PERIOD_Q8 = 2304          # must match the firmware
COS_N = 256


def node_cos_table():
    """Bit-identical to what beltInit() builds on the AVR."""
    return np.rint(np.cos(2 * np.pi * np.arange(COS_N) / COS_N) * 4096.0)


def solve_float(r, amp, win):
    """The same grid search, in float, on the node's own residuals."""
    tab = node_cos_table()
    k = np.arange(win)
    best = None
    for p in range(COS_N):
        idx = ((k * 65536 // BELT_PERIOD_Q8) + p) % COS_N
        wave = amp * tab[idx] / 16.0            # Q8, as on the node
        off = np.mean(r[:win] * 256.0 - wave)
        e = r[:win] * 256.0 - wave - off
        s = float(np.sum(e * e))
        if best is None or s < best[0]:
            best = (s, p, off)
    return best[1], best[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--interval", type=int, default=400)
    ap.add_argument("--laps", type=int, default=14)
    ap.add_argument("--win", type=int, default=6)
    ap.add_argument("--dir", type=int, default=0, choices=(0, 1))
    ap.add_argument("--budget", type=int, default=21447)
    args = ap.parse_args()

    node = Node(args.port, args.baud, False)
    try:
        node.cmd("e 1", terminator="# en")
        node.cmd(f"d {args.dir}", terminator="# dir")
        secs = args.laps * 16500 * args.interval / 1e6 + 90
        lines = node.cmd(f"p {args.interval} {args.laps} {args.budget} {args.win}",
                         terminator="# PRMS", timeout=secs * 2)
    finally:
        node.close()

    hdr = [t for t in lines if t.startswith("# PHASE")]
    rms = [t for t in lines if t.startswith("# PRMS")]
    laps = [t for t in lines if t.startswith("# PLAP")]
    if not hdr or "found=1" not in hdr[0]:
        print("\n".join(lines))
        raise SystemExit("node did not complete the sweep")

    h = parse_kv(hdr[0].replace("# PHASE ", ""))
    amp = int(h["amp"])
    node_phase = int(h["phase"])

    r = np.array([float(t.split(" r=")[1].split()[0]) for t in laps])
    ec = np.array([float(t.split(" ec=")[1].split()[0]) for t in laps])

    pc_phase, pc_off = solve_float(r, amp, args.win)

    print(f"\n=== node vs PC, {len(r)} laps, window {args.win}, "
          f"dir {args.dir}, amp {amp} ===\n")
    print(f"  phase index   node {node_phase:4d}    PC {pc_phase:4d}"
          f"    diff {abs(node_phase - pc_phase):d}/256"
          f"  ({abs(node_phase-pc_phase)*360/256:.1f} deg of belt cycle)")
    print(f"  offset Q8     node {int(h['offq8']):9d}    PC {pc_off:9.1f}"
          f"    diff {abs(int(h['offq8']) - pc_off)/256.0:.3f} steps")

    # Re-derive the corrected residuals in float and compare to the node's.
    tab = node_cos_table()
    k = np.arange(len(r))
    idx = ((k * 65536 // BELT_PERIOD_Q8) + pc_phase) % COS_N
    ec_pc = (r * 256.0 - amp * tab[idx] / 16.0 - pc_off) / 256.0
    d = ec - ec_pc
    print(f"  per-lap corrected residual, node minus PC:"
          f"  max {np.abs(d).max():.3f} steps, rms {np.sqrt(np.mean(d*d)):.3f}")

    print()
    print("  " + rms[0][2:] if rms else "  (no rms line)")
    if rms:
        m = parse_kv(rms[0].replace("# PRMS ", ""))
        print(f"\n  held-out laps: {m['n']}, corrected {m['corrected']} steps "
              f"({m['degc']} deg), uncorrected {m['uncorrected']}, "
              f"gain {m['gain']}x")


if __name__ == "__main__":
    main()
