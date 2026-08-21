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

Steps per revolution at the output shaft is ~16497.7 (45.827 steps/deg, belt
ratio 5.156), but read the uncertainty carefully. Over 15 laps, three different
reductions of the SAME four captures give:

    endpoint over the full 14-lap baseline    16491.4
    least-squares slope through all 15 dips   16495.3
    mean over 9-lap baselines                 16497.7

Each has an internal repeatability under 1 step, and they disagree by 6. That
spread is the ~8.8-lap periodic error biasing each reduction differently, and
it means internal precision here badly overstates accuracy: the honest figure
is 16497.7 +-3 steps (+-0.07 deg), not the sub-step number any single reduction
reports.

The 9-lap figure is the one to use, on two grounds: a 9-lap baseline is one
full period of the error, so the periodic term cancels by construction; and it
is the only reduction under which the forward and reverse runs agree (0.34
steps apart, against 2.75 for the full baseline). Since a revolution must
return to the same physical angle, direction agreement is a correctness check,
not a coincidence.

Settling this properly wants ~30 revolutions, i.e. two full periods. See
hall_revs.py.

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
