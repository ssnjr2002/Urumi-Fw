"""
jog.py — fixed-distance, ramped manual jog for testing.

Generates a single straight move with a trapezoidal velocity profile (accel
from rest -> cruise -> decel to rest), so the machine starts and ends smoothly.
Works on any axis (X/Y/Z/A) or a diagonal, using the per-axis map and
calibration from pipeline/config.py. Streams over the Go-Back-N sender.

Usage:
  python jog.py --port COM8 --axis x --dist 10                 # +10 mm on X
  python jog.py --port COM8 --axis y --dist -5  --feed 15      # -5 mm on Y, 15 mm/s
  python jog.py --port COM8 --axis a --dist 90                 # +90 deg on A
  python jog.py --port COM8 --dx 10 --dy 5                     # diagonal jog (mm)

Distances are mm for linear axes, degrees for the rotary A axis. Feed/accel are
in the same units/s and units/s^2.
"""

import sys, os, argparse, math

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pipeline", "stages"))

from serialise import pack_microsegment, MSEG_FLAG_PATH_END, MSEG_FLAG_NONE
from sender import Sender
from config import default as _config_default

try:
    import serial
except ImportError:
    serial = None

from collections import namedtuple
_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])

# Axis name -> (MicroSegment field index, config AxisConfig attribute)
_AXES = ["x", "y", "z", "a"]


def _axis_cfg(machine, name):
    return getattr(machine, name)


def make_jog(steps, feed_sps, accel_sps2, f_cpu, v_start_sps=50.0):
    """
    Trapezoidal jog as a list of MicroSegment packets.

    steps      : (sx, sy, sz, sa) signed target step counts
    feed_sps   : cruise step rate of the MAJOR axis (steps/s)
    accel_sps2 : acceleration of the major axis (steps/s^2)
    One MicroSegment per major-axis step; minor axes are host-side Bresenham
    distributed. Velocity follows v = sqrt(v0^2 + 2*a*d), giving a smooth ramp.
    """
    sx, sy, sz, sa = steps
    abss = [abs(sx), abs(sy), abs(sz), abs(sa)]
    major = max(abss)
    if major == 0:
        return []

    signs = [(1 if s >= 0 else -1) for s in steps]
    v0 = max(1.0, min(v_start_sps, feed_sps))

    # Trapezoid geometry (in major-axis steps)
    d_acc = (feed_sps**2 - v0**2) / (2.0 * accel_sps2)
    if 2 * d_acc > major:  # triangular — never reach cruise
        peak = math.sqrt(v0**2 + accel_sps2 * major)
        d_acc = (peak**2 - v0**2) / (2.0 * accel_sps2)
    d_dec = d_acc

    err = [major // 2] * 4   # Bresenham accumulators for minor axes
    packets = []

    for n in range(major):
        # velocity at this step
        if n < d_acc:
            v = math.sqrt(v0**2 + 2.0 * accel_sps2 * n)
        elif n >= major - d_dec:
            v = math.sqrt(v0**2 + 2.0 * accel_sps2 * (major - n))
        else:
            v = feed_sps
        v = max(v, v0)
        interval = max(1, min(int(f_cpu / v), f_cpu))

        # which axes step this tick — major always, minors via Bresenham
        delta = [0, 0, 0, 0]
        for ax in range(4):
            if abss[ax] == 0:
                continue
            if abss[ax] == major:
                delta[ax] = signs[ax]
            else:
                err[ax] += abss[ax]
                if err[ax] >= major:
                    err[ax] -= major
                    delta[ax] = signs[ax]

        flags = MSEG_FLAG_PATH_END if n == major - 1 else MSEG_FLAG_NONE
        packets.append(pack_microsegment(
            _MS(dx=delta[0], dy=delta[1], dz=delta[2], da=delta[3],
                interval=interval, flags=flags)))

    return packets


def main():
    cfg = _config_default()
    machine = cfg.machine

    ap = argparse.ArgumentParser(description="Fixed-distance ramped jog")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--axis", choices=_AXES, help="Single-axis jog")
    ap.add_argument("--dist", type=float, help="Distance (mm, or deg for A) for --axis")
    ap.add_argument("--dx", type=float, default=0.0, help="X distance mm (diagonal)")
    ap.add_argument("--dy", type=float, default=0.0, help="Y distance mm (diagonal)")
    ap.add_argument("--feed",  type=float, default=20.0, help="Cruise feed units/s (default 20)")
    ap.add_argument("--accel", type=float, default=200.0, help="Accel units/s^2 (default 200)")
    ap.add_argument("--window", type=int, default=16)
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()

    # Resolve the jog into per-axis step targets + the set of nodes to enable
    steps = [0, 0, 0, 0]
    nodes = set()
    major_axis = None

    if args.axis:
        if args.dist is None:
            ap.error("--axis requires --dist")
        ax = _axis_cfg(machine, args.axis)
        s = int(round(args.dist * ax.steps_per_unit)) * (-1 if ax.invert else 1)
        steps[_AXES.index(args.axis)] = s
        nodes.add(ax.node)
        major_axis = args.axis
    else:
        if args.dx == 0.0 and args.dy == 0.0:
            ap.error("nothing to jog: pass --axis/--dist or --dx/--dy")
        steps[0] = int(round(args.dx * machine.x.steps_per_unit)) * (-1 if machine.x.invert else 1)
        steps[1] = int(round(args.dy * machine.y.steps_per_unit)) * (-1 if machine.y.invert else 1)
        if steps[0]: nodes.add(machine.x.node)
        if steps[1]: nodes.add(machine.y.node)
        # major axis for feed scaling = the longer leg
        major_axis = "x" if abs(steps[0]) >= abs(steps[1]) else "y"

    spu = _axis_cfg(machine, major_axis).steps_per_unit
    feed_sps  = args.feed  * spu
    accel_sps2 = args.accel * spu

    packets = make_jog(tuple(steps), feed_sps, accel_sps2, machine.f_cpu)
    if not packets:
        print("Nothing to send (zero steps).", file=sys.stderr)
        sys.exit(1)

    print(f"Jog: steps={steps}  feed={args.feed} accel={args.accel}  "
          f"-> {len(packets)} MicroSegments, nodes {sorted(nodes)}", file=sys.stderr)

    if serial is None:
        print("pyserial not installed — pip install pyserial", file=sys.stderr)
        sys.exit(1)

    import time
    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        time.sleep(0.5)
        ser.reset_input_buffer()
        # Enable the nodes this jog drives (stream bytes are dropped otherwise)
        for node in sorted(nodes):
            ser.write(f"enable {node}\n".encode())
            ser.flush()
            time.sleep(0.2)
        ser.reset_input_buffer()

        sender = Sender(ser, window=args.window, verbose=args.verbose)
        try:
            ok = sender.send_stream(packets)
        finally:
            sender.stop()
            sender.report()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
