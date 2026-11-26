#!/usr/bin/env python3
"""hall_home.py — test the node's own homing, not the PC's idea of it.

Throwaway bench tool. Drives the `h` command in src/scratch/hall_capture.cpp,
which finds the index with the SAME fixed-point, 4:1-decimated est_mirror that
would run on the node, and reports it in an absolute step frame the firmware
maintains across trials.

Three tests, in order of what they can prove:

  --repeat N    Home N times, each from a different random start angle.
                Reduces every index position modulo one revolution and reports
                the scatter. This is PRECISION: does homing land in the same
                place regardless of where it started?

  --crosscheck  Have the node emit the very window it just reduced, run the
                float est_mirror from hall_analyze over those identical
                samples, and difference the two answers. Same data, so any
                disagreement is purely the fixed-point/decimated arithmetic --
                no mechanical variability in the way. This is the only test
                here that isolates the implementation from the mechanism.

  --angle DEG   Home, command exactly DEG, home again, and report what the
                sensor says actually happened versus what was asked for. This
                is the one that catches a wrong steps-per-degree.

WHAT NONE OF THIS PROVES. Every test here asks the Hall sensor to check work
the Hall sensor did. It cannot see a magnet that has crept, a steps/rev that is
wrong (homing stays perfectly repeatable while every commanded angle is short),
or whether "home" is the angle anyone thinks it is. Perfect scores here are
consistent with an axis that is consistently wrong. External verification --
a mark on the belt against a mark on a static part -- is not belt-and-braces,
it is the only source of truth in the loop.
"""
import argparse
import random
import statistics
import sys
import time

import numpy as np
import serial

import hall_analyze as ha

STEPS_PER_REV = 16498          # measured; see hall_capture.py's docstring


class Node:
    def __init__(self, port, baud=500000, verbose=False):
        self.ser = serial.Serial(port, baud, timeout=2)
        self.verbose = verbose
        time.sleep(2.0)                      # board resets on DTR
        self.ser.reset_input_buffer()

    def cmd(self, line, terminator=None, timeout=120):
        self.ser.reset_input_buffer()
        self.ser.write((line + "\n").encode())
        out, deadline = [], time.time() + timeout
        while time.time() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            t = raw.decode("ascii", "replace").strip()
            if not t:
                continue
            out.append(t)
            if self.verbose and t.startswith("#"):
                print("   " + t, file=sys.stderr)
            if terminator is None or t.startswith(terminator):
                if terminator is not None and t.startswith(terminator):
                    break
                if terminator is None:
                    break
        return out

    def close(self):
        try:
            self.ser.write(b"e 0\n")         # never leave the driver energised
            time.sleep(0.2)
        finally:
            self.ser.close()


def parse_kv(line):
    d = {}
    for tok in line.split():
        if "=" in tok:
            k, _, v = tok.partition("=")
            try:
                d[k] = float(v) if "." in v else int(v)
            except ValueError:
                d[k] = v
    return d


def home(node, interval, budget, emit=False):
    """One homing run. Returns the parsed '# HOME' fields, plus the emitted
    window if one was asked for."""
    term = "# WIN end" if emit else "# HOME"
    lines = node.cmd(f"h {interval} {budget} {1 if emit else 0}", terminator=term)

    res, win, in_win = None, [], False
    for t in lines:
        if t.startswith("# HOME"):
            res = parse_kv(t)
            res["_ok"] = "found=1" in t
        elif t.startswith("# WIN start"):
            res = res or {}
            res.update(parse_kv(t))
            in_win = True
        elif t.startswith("# WIN end"):
            in_win = False
        elif in_win:
            try:
                win.append(int(t))
            except ValueError:
                pass
    return res, win


def test_repeat(node, args):
    print(f"\n=== repeatability: {args.repeat} homings from random starts ===")
    print(f"    budget {args.budget} steps, {args.interval} us/step, "
          f"dir {args.dir}\n")
    print("    trial     start off      index abs     index mod rev")

    node.cmd(f"d {args.dir}", terminator="# dir")
    rows = []
    for k in range(args.repeat):
        # The offset must move the SAME way the sweep will run. Move against
        # the sweep and homing simply turns around and re-finds the dip it just
        # left, so the axis oscillates over a few degrees and every trial
        # measures the same lap -- which holds belt phase constant and hides
        # the belt error instead of sampling it.
        off = random.randint(0, STEPS_PER_REV - 1) * (-1 if args.dir else 1)
        node.cmd(f"m {off} {args.interval}", terminator="# MOVE")
        res, _ = home(node, args.interval, args.budget)
        if not res or not res.get("_ok"):
            print(f"    {k:5d}   {off:11d}   NOT FOUND")
            continue
        idx = res["centre"]
        rows.append((off, idx, idx % STEPS_PER_REV))
        print(f"    {k:5d}   {off:11d}   {idx:12.1f}   {idx % STEPS_PER_REV:12.1f}")

    if len(rows) < 2:
        print("\n    too few successful homings to score")
        return

    m = [r[2] for r in rows]
    # Guard the wrap: if the index sits near 0 or near STEPS_PER_REV the
    # modulo splits one cluster into two, and the sd would be meaningless.
    if max(m) - min(m) > STEPS_PER_REV / 2:
        m = [x if x < STEPS_PER_REV / 2 else x - STEPS_PER_REV for x in m]

    sd = statistics.stdev(m)
    print(f"\n    n = {len(m)}")
    print(f"    spread  {max(m) - min(m):8.2f} steps "
          f"({(max(m) - min(m)) * 360 / STEPS_PER_REV:.4f} deg)")
    print(f"    sd      {sd:8.2f} steps "
          f"({sd * 360 / STEPS_PER_REV:.4f} deg)")
    print("\n    NOTE: this is precision only. A crept magnet or a wrong "
          "steps/rev\n    leaves this number looking perfect.")


def test_crosscheck(node, args):
    print(f"\n=== cross-check: node fixed-point vs PC float, same samples ===")
    print("    identical data both sides, so any difference is arithmetic\n")
    print("    trial      node      PC float      diff (steps)")

    node.cmd(f"d {args.dir}", terminator="# dir")
    diffs = []
    for k in range(args.crosscheck):
        off = random.randint(0, STEPS_PER_REV - 1) * (-1 if args.dir else 1)
        node.cmd(f"m {off} {args.interval}", terminator="# MOVE")
        res, win = home(node, args.interval, args.budget, emit=True)
        if not res or not res.get("_ok") or not win:
            print(f"    {k:5d}   NOT FOUND")
            continue

        v = np.asarray(win, float)
        # The node used a running max as its baseline; use its reported value
        # so the two reductions genuinely share every input.
        x = np.arange(len(v), dtype=float)
        c = ha.est_mirror(x, v, float(res["baseline"]))

        sign = res["sign"]
        decim = res["decim"]
        pc_idx = res["start"] + c * decim * sign
        d = res["centre"] - pc_idx
        diffs.append(d)
        print(f"    {k:5d}   {res['centre']:10.1f}   {pc_idx:10.1f}   {d:+12.2f}")

    if diffs:
        print(f"\n    mean {statistics.mean(diffs):+.2f} steps, "
              f"max |diff| {max(abs(d) for d in diffs):.2f} steps "
              f"({max(abs(d) for d in diffs) * 360 / STEPS_PER_REV:.4f} deg)")
        print("    A bias here is the decimation or the fixed point, not the axis.")


def test_multi(node, args):
    """Does the belt-cancelling multi-lap home actually beat the single-lap one?

    Both numbers come out of the SAME sweep -- 'raw' is the first lap's index,
    'ref' is that index with the periodic term removed -- so this is a paired
    comparison with no run-to-run mechanical difference between the two arms.
    """
    laps = args.multi_laps
    print(f"\n=== multi-lap home: {args.multi} trials x {laps} laps ===")
    print("    raw = first lap alone; ref = belt term cancelled over one period")
    print("    both from the same sweep, so this is a paired comparison\n")
    print("    trial          raw            ref         spr    resid sd")

    node.cmd(f"d {args.dir}", terminator="# dir")
    raws, refs, sprs = [], [], []
    for k in range(args.multi):
        off = random.randint(0, STEPS_PER_REV - 1) * (-1 if args.dir else 1)
        node.cmd(f"m {off} {args.interval}", terminator="# MOVE")
        lines = node.cmd(f"H {args.interval} {laps} {args.budget}",
                         terminator="# LAP", timeout=60 + laps * 30)
        hit = [t for t in lines if t.startswith("# HOMEN")]
        if not hit or "found=1" not in hit[0]:
            print(f"    {k:5d}   FAILED")
            continue
        d = parse_kv(hit[0])
        raws.append(d["raw"]); refs.append(d["ref"]); sprs.append(d["spr"])
        print(f"    {k:5d}   {d['raw']:12.1f}   {d['ref']:12.1f}   "
              f"{d['spr']:9.1f}   {d['residsd']:9.2f}")

    if len(raws) < 2:
        print("\n    too few successful runs to score")
        return

    def scatter(vals):
        m = [v % STEPS_PER_REV for v in vals]
        if max(m) - min(m) > STEPS_PER_REV / 2:
            m = [x if x < STEPS_PER_REV / 2 else x - STEPS_PER_REV for x in m]
        return statistics.stdev(m)

    sr, sf = scatter(raws), scatter(refs)
    deg = 360.0 / STEPS_PER_REV
    print(f"\n    n = {len(raws)}")
    print(f"    raw  sd {sr:7.2f} steps ({sr * deg:.4f} deg)")
    print(f"    ref  sd {sf:7.2f} steps ({sf * deg:.4f} deg)")
    if sf > 0:
        print(f"    improvement {sr / sf:.1f}x")
    print(f"    steps/rev  mean {statistics.mean(sprs):.1f}"
          + (f"  sd {statistics.stdev(sprs):.1f}" if len(sprs) > 1 else ""))


def test_angle(node, args):
    deg = args.angle
    steps = int(round(deg * STEPS_PER_REV / 360.0))
    print(f"\n=== commanded angle: {deg} deg = {steps} steps at "
          f"{STEPS_PER_REV} steps/rev ===")
    print("    home, move, home again, and ask the sensor what really happened\n")

    node.cmd(f"d {args.dir}", terminator="# dir")
    res0, _ = home(node, args.interval, args.budget)
    if not res0 or not res0.get("_ok"):
        print("    first homing failed"); return

    node.cmd(f"m {steps} {args.interval}", terminator="# MOVE")

    res1, _ = home(node, args.interval, args.budget)
    if not res1 or not res1.get("_ok"):
        print("    second homing failed"); return

    # Both index positions are the SAME physical angle, so their difference is
    # a whole number of revolutions -- in true steps/rev, whatever that is.
    delta = res1["centre"] - res0["centre"]
    laps = round(delta / STEPS_PER_REV)
    print(f"    index moved {delta:.1f} steps over {laps} revolution(s)")
    if laps:
        measured = delta / laps
        print(f"    implied steps/rev {measured:.1f} "
              f"(assumed {STEPS_PER_REV})")
        err = (measured - STEPS_PER_REV) / STEPS_PER_REV
        print(f"    scale error {err * 100:+.3f}%  ->  over one commanded "
              f"revolution that is {err * 360:+.2f} deg")
    print("\n    Confirm against a physical mark. The sensor cannot tell you "
          "whether\n    the axis went where you asked, only where the magnet is.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--interval", type=int, default=400,
                    help="us/step; speed bias measured at 0.94 steps across a "
                         "2.5x change, so pick this for time not accuracy")
    ap.add_argument("--budget", type=int, default=int(STEPS_PER_REV * 1.3),
                    help="max steps to sweep looking for a complete dip. Worst "
                         "case is one full rev plus a dip width; over-budgeting "
                         "a rotary axis is free, there is no hard stop.")
    ap.add_argument("--dir", type=int, default=0, choices=(0, 1))
    ap.add_argument("--repeat", type=int, default=0)
    ap.add_argument("--crosscheck", type=int, default=0)
    ap.add_argument("--multi", type=int, default=0,
                    help="trials of the belt-cancelling multi-lap home")
    ap.add_argument("--multi-laps", type=int, default=9,
                    help="laps per multi-lap home. One full period of the belt "
                         "error, measured at 8.8-9.1 laps, so 9 is the value "
                         "that cancels it")
    ap.add_argument("--angle", type=float, default=None)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    if args.seed is not None:
        random.seed(args.seed)
    if not (args.repeat or args.crosscheck or args.multi or args.angle is not None):
        raise SystemExit("pick at least one of --repeat, --crosscheck, "
                         "--multi, --angle")

    node = Node(args.port, args.baud, args.verbose)
    try:
        node.cmd("e 1", terminator="# en")
        if args.crosscheck:
            test_crosscheck(node, args)
        if args.repeat:
            test_repeat(node, args)
        if args.multi:
            test_multi(node, args)
        if args.angle is not None:
            test_angle(node, args)
    finally:
        node.close()


if __name__ == "__main__":
    main()
