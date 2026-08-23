#!/usr/bin/env python3
"""hall_capture.py — drive src/scratch/hall_capture.cpp and save a run to CSV.

Throwaway bench tool, paired with the firmware of the same name. Deleted along
with it once an index estimator has been chosen.

  python hall_capture.py --port COM15 --interval 1000 --steps 128000 --dir 0
  python hall_capture.py --port COM15 --interval 1000 --steps 128000 --dir 1
  python hall_capture.py --port COM15 --interval  400 --steps 128000 --dir 0

Sweep enough revolutions to get many laps of the same physical index: the whole
point is lap-to-lap scatter, which needs laps. The board is strapped to 1/16
microstepping in solder, so the motor takes 3200 steps/rev.

Steps per revolution at the output shaft is 16497.9 +-0.3 (45.8275 steps/deg,
belt ratio 5.15559). Getting to that number took an estimator change, not more
data, and the story is worth keeping because it is a general trap.

Three obvious reductions of the SAME four captures disagreed by 6 steps:

    endpoint over the full 14-lap baseline    16491.4
    least-squares slope through all 15 dips   16495.3
    mean over 9-lap baselines                 16497.7

Each repeated internally to under a step, which made all three look precise and
one of them wrong. In fact all three were biased, by the ~8.8-lap periodic belt
error riding on the dip positions. A sinusoid spanning a non-integer number of
periods has non-zero correlation with a ramp, so ANY slope taken through those
points inherits some of it -- and each reduction inherits a different amount,
which is exactly the 6-step spread.

The fix is to stop treating the belt term as noise to be averaged away and fit
it alongside the slope:

    pos[k] = a + b*k + c*cos(2 pi k/P) + d*sin(2 pi k/P)

scanning P. Then b is the slope with the belt term projected out rather than
smeared into it. Across all four captures -- two speeds crossed with two
directions, so four largely independent measurements -- that gives

    16497.73  16498.09  16497.57  16498.08     mean 16497.87, sd 0.26

which is a tenfold improvement over the +-3 the three-way disagreement forced,
from the same bytes on disk. It also predicts the old biases correctly (-6.2
for the endpoint, -2.6 for the plain slope), which is the check that says the
model is right rather than merely tighter.

The same fit pins the belt period at 8.814 +-0.052 laps, amplitude ~46.6 steps
forward and ~29 reverse, and leaves 5-9 steps of residual.

Two things this DISPROVED, recorded so they do not get re-proposed:

  * There is no exact tooth ratio to find. 5.15559 is not a low-denominator
    rational; the closest is 232/45 = 5.155556, needing a 45T motor pulley and
    a 232T output, and nothing simpler lands within 4 sems. Under a single
    2 mm-pitch stage no integer tooth set reproduces the ratio AND the belt
    period AND leaves room for the pulleys not to intersect. That is what a
    deliberately compliant printed belt should look like: engagement is not a
    clean kinematic constraint, so the ratio is a real number, not a fraction.
    16497.9 +-0.3 IS the answer; do not go hunting for a nicer one.

  * There is no motor-rotor signature. Adding a term at the period a rotor
    error would alias to buys an amplitude no larger than the same term at
    control periods with no physical meaning -- i.e. it is fitting noise.

More revolutions would still help the belt period, which is the weakest number
here. See hall_revs.py.

Vary two things across runs, because they answer different questions:

  --interval  scatter measures PRECISION, but only a speed change exposes a
              speed-dependent BIAS, which scatter is blind to.

  --dir       reversing flips the SPATIAL axis but not any TIME lag, so the two
              separate: the mean of the forward and reverse index positions is
              the true spatial centre with lag cancelled, and half their
              difference is the lag and backlash combined. The A axis turns both
              ways during a job anyway, so the estimator has to work both ways.
"""
import argparse
import pathlib
import sys
import time

import serial
from serial.tools import list_ports


def pick_port(explicit):
    if explicit:
        return explicit
    ports = [p.device for p in list_ports.comports()]
    if len(ports) == 1:
        print(f"# using only available port: {ports[0]}", file=sys.stderr)
        return ports[0]
    raise SystemExit(f"--port required; available: {ports or 'none'}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=500000)
    ap.add_argument("--interval", type=int, required=True,
                    help="microseconds per step (sweep speed)")
    ap.add_argument("--steps", type=int, help="total steps; or use --revs")
    ap.add_argument("--revs", type=float, help="revolutions (needs --steps-per-rev)")
    ap.add_argument("--steps-per-rev", type=int, default=16498,
                    help="output-shaft steps per revolution, for --revs "
                         "(measured on node 4 at 1/16 microstepping)")
    ap.add_argument("--dir", type=int, default=0, choices=(0, 1))
    ap.add_argument("--preroll", type=int, default=2000,
                    help="steps taken but not emitted, so the capture starts at "
                         "settled speed instead of from standstill. Set 0 to "
                         "measure the belt start-up transient instead — but then "
                         "park the axis AWAY from the magnet first, or the "
                         "transient sits on top of a dip and cannot be read.")
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    if args.steps is None:
        if args.revs is None:
            raise SystemExit("need --steps or --revs")
        args.steps = int(round(args.revs * args.steps_per_rev))

    # Captures land next to this script, not in whatever directory it was run
    # from, so a session's runs stay together for the analyzer.
    out = args.out or str(pathlib.Path(__file__).resolve().parent /
                          f"hall_i{args.interval}_d{args.dir}_n{args.steps}.csv")
    port = pick_port(args.port)

    # The firmware paces off micros() deadlines, so a run takes very close to
    # interval*steps. Allow generous slack on top before giving up.
    expect_s = args.interval * (args.steps + args.preroll) / 1e6
    print(f"# {port} @ {args.baud} | {args.steps} steps @ {args.interval}us "
          f"= ~{expect_s:.1f}s -> {out}", file=sys.stderr)

    with serial.Serial(port, args.baud, timeout=2) as ser, open(out, "w") as fh:
        time.sleep(2.0)              # board resets on DTR; wait for the banner
        ser.reset_input_buffer()

        for cmd in (f"d {args.dir}", "e 1"):
            ser.write((cmd + "\n").encode())
            time.sleep(0.2)
        ser.reset_input_buffer()

        ser.write(f"r {args.interval} {args.steps} {args.preroll}\n".encode())

        n, started, deadline = 0, False, time.time() + expect_s + 30
        try:
            while time.time() < deadline:
                raw = ser.readline()
                if not raw:
                    continue
                line = raw.decode("ascii", "replace").strip()
                if not line:
                    continue

                if line.startswith("# BEGIN"):
                    started = True
                    fh.write(line + "\n")
                    fh.write("step,adc\n")
                    print(line, file=sys.stderr)
                    continue
                if line.startswith("# END"):
                    fh.write(line + "\n")
                    print(f"{line}  (captured {n})", file=sys.stderr)
                    break
                if line.startswith("#"):
                    print(line, file=sys.stderr)
                    continue

                if started:
                    fh.write(line + "\n")
                    n += 1
                    if n % 5000 == 0:
                        print(f"\r# {n}/{args.steps}", end="", file=sys.stderr)
            else:
                print("\n# TIMEOUT — partial capture kept", file=sys.stderr)
        finally:
            ser.write(b"e 0\n")      # never leave the driver energised
            time.sleep(0.2)

    if n == 0:
        raise SystemExit("no samples captured — check wiring, baud, and that "
                         "the hall_capture firmware is flashed")
    print(f"\n# wrote {n} samples to {out}", file=sys.stderr)


if __name__ == "__main__":
    main()
