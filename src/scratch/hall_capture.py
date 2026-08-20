#!/usr/bin/env python3
"""hall_capture.py — drive src/scratch/hall_capture.cpp and save a run to CSV.

Throwaway bench tool, paired with the firmware of the same name. Deleted along
with it once an index estimator has been chosen.

  python hall_capture.py --port COM12 --interval 1000 --revs 20 --steps-per-rev 6400
  python hall_capture.py --port COM12 --interval 1000 --steps 128000 -o run_fast.csv

Sweep enough revolutions to get many laps of the same physical index: the whole
point is lap-to-lap scatter, which needs laps. Run the same sweep at two or
three different intervals as well — scatter measures precision, but only a
speed change exposes a speed-dependent bias.
"""
import argparse
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
    ap.add_argument("--steps-per-rev", type=int, default=6400,
                    help="output-shaft steps per revolution, for --revs")
    ap.add_argument("--dir", type=int, default=0, choices=(0, 1))
    ap.add_argument("-o", "--out", default=None)
    args = ap.parse_args()

    if args.steps is None:
        if args.revs is None:
            raise SystemExit("need --steps or --revs")
        args.steps = int(round(args.revs * args.steps_per_rev))

    out = args.out or f"hall_i{args.interval}_d{args.dir}_n{args.steps}.csv"
    port = pick_port(args.port)

    # The firmware paces off micros() deadlines, so a run takes very close to
    # interval*steps. Allow generous slack on top before giving up.
    expect_s = args.interval * args.steps / 1e6
    print(f"# {port} @ {args.baud} | {args.steps} steps @ {args.interval}us "
          f"= ~{expect_s:.1f}s -> {out}", file=sys.stderr)

    with serial.Serial(port, args.baud, timeout=2) as ser, open(out, "w") as fh:
        time.sleep(2.0)              # board resets on DTR; wait for the banner
        ser.reset_input_buffer()

        for cmd in (f"d {args.dir}", "e 1"):
            ser.write((cmd + "\n").encode())
            time.sleep(0.2)
        ser.reset_input_buffer()

        ser.write(f"r {args.interval} {args.steps}\n".encode())

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
