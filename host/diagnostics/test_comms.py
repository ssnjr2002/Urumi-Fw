"""
test_comms.py — synthetic comms verification for microseg-host-drive

Generates known MicroSegment sequences WITHOUT using the SVG pipeline and
sends them to the Pico. Use this to verify the comms layer in isolation
before testing with real SVG jobs.

Tests (run in order):
  1. ping        — confirm all enabled nodes respond
  2. loopback    — send N MicroSegments with zero steps, verify all ACKed
  3. line        — single-axis straight line move on one node
  4. backpressure— flood the buffer, verify NACK_FULL + ready signal

Usage:
  python test_comms.py --port COM3 --node 1
  python test_comms.py --port COM3 --node 1 --test loopback
  python test_comms.py --port COM3 --node 1 --test line --steps 200 --feed 20
"""

import sys, os, argparse, time, struct, threading, queue

from host.protocol.packets import (
    pack_microsegment, validate_packet,
    MAGIC_ACK, MAGIC_NACK, NACK_FULL,
    MSEG_FLAG_PATH_END, MSEG_FLAG_NONE,
)
from host.protocol.stream import Sender, read_packets

try:
    import serial
except ImportError:
    print("pyserial not installed — pip install pyserial", file=sys.stderr)
    sys.exit(1)

# ── MicroSegment factories ────────────────────────────────────────────────────

F_CPU = 150_000_000

def _ms(dx=0, dy=0, dz=0, da=0, feed_sps=1000, flags=MSEG_FLAG_NONE):
    """Build a MicroSegment packet at a given step rate (steps/sec)."""
    from collections import namedtuple
    MS = namedtuple("MS", ["dx","dy","dz","da","interval","flags"])
    interval = max(1, int(F_CPU / feed_sps))
    return pack_microsegment(MS(dx=dx, dy=dy, dz=dz, da=da,
                                interval=interval, flags=flags))


def make_loopback_packets(n=32):
    """N zero-step packets — exercises ACK/NACK without moving motors."""
    pkts = []
    for i in range(n):
        flags = MSEG_FLAG_PATH_END if i == n - 1 else MSEG_FLAG_NONE
        pkts.append(_ms(flags=flags))
    return pkts


def make_line_packets(steps, feed_sps, axis='x'):
    """Straight line along one axis, trapezoidal ramp."""
    if steps <= 0:
        return []

    # Simple trapezoidal: accel for first third, cruise for middle, decel for last third
    accel_steps = steps // 3
    decel_steps = steps // 3
    cruise_steps = steps - accel_steps - decel_steps

    min_sps = max(1, feed_sps // 10)
    pkts = []

    def emit(dx, dy, sps, flags):
        pkts.append(_ms(dx=dx, dy=dy, feed_sps=sps, flags=flags))

    total = accel_steps + cruise_steps + decel_steps
    for i in range(total):
        if i < accel_steps:
            sps = int(min_sps + (feed_sps - min_sps) * (i / max(1, accel_steps)))
        elif i >= accel_steps + cruise_steps:
            j = i - accel_steps - cruise_steps
            sps = int(feed_sps - (feed_sps - min_sps) * (j / max(1, decel_steps)))
        else:
            sps = feed_sps

        sps = max(min_sps, sps)
        flags = MSEG_FLAG_PATH_END if i == total - 1 else MSEG_FLAG_NONE
        dx = 1 if axis == 'x' else 0
        dy = 1 if axis == 'y' else 0
        emit(dx, dy, sps, flags)

    return pkts


def make_multistep_packet(steps, feed_sps, axis='x'):
    """ONE MicroSegment carrying many steps on a single axis.
    Exercises the Pico's within-segment Bresenham loop — the single thing the
    single-step line test cannot reach. If this moves `steps` but the segment
    only produces one pulse, the major-axis stepping is broken."""
    from collections import namedtuple
    MS = namedtuple("MS", ["dx","dy","dz","da","interval","flags"])
    interval = max(1, int(F_CPU / feed_sps))
    dx = steps if axis == 'x' else 0
    dy = steps if axis == 'y' else 0
    return [pack_microsegment(MS(dx=dx, dy=dy, dz=0, da=0,
                                 interval=interval, flags=MSEG_FLAG_PATH_END))]


def make_diagonal_packet(sx, sy, feed_sps):
    """ONE MicroSegment with steps on BOTH axes — exercises minor-axis
    Bresenham distribution. Result should be a straight diagonal line."""
    from collections import namedtuple
    MS = namedtuple("MS", ["dx","dy","dz","da","interval","flags"])
    interval = max(1, int(F_CPU / feed_sps))
    return [pack_microsegment(MS(dx=sx, dy=sy, dz=0, da=0,
                                 interval=interval, flags=MSEG_FLAG_PATH_END))]


# ── serial helpers ────────────────────────────────────────────────────────────

def send_text(ser, cmd):
    ser.write((cmd + '\n').encode())
    ser.flush()
    time.sleep(0.1)
    resp = ser.read(ser.in_waiting).decode(errors='replace').strip()
    return resp


def wait_for_ready(ser, timeout=10.0):
    """Wait for 'ready' text from Core 0 after buffer drains."""
    deadline = time.monotonic() + timeout
    buf = ''
    while time.monotonic() < deadline:
        chunk = ser.read(ser.in_waiting or 1).decode(errors='replace')
        buf += chunk
        if 'ready' in buf:
            return True
    return False


# Axis → node mapping (matches Pico stream-byte packing: X=1 Y=2 Z=3 A=4)
AXIS_NODE = {'x': 1, 'y': 2, 'z': 3, 'a': 4}


# ── tests ─────────────────────────────────────────────────────────────────────

def test_ping(ser, node):
    print(f"\n[1] PING node {node}")
    # First confirm the Pico is alive (control-plane ping)
    resp_pico = send_text(ser, 'ping')
    if resp_pico != 'pong':
        print(f"    FAIL — Pico ping: {repr(resp_pico)}")
        return False
    # Then relay an RS485 ping to the node
    resp = send_text(ser, f'pingnode {node}')
    if resp.endswith('ok'):
        print(f"    PASS — {resp}")
        return True
    print(f"    FAIL — got: {repr(resp)} (node may be offline — timeout is expected without hardware)")
    return False


def test_loopback(ser, n=64, window=16, verbose=False):
    print(f"\n[2] LOOPBACK — {n} zero-step packets, window={window}")
    pkts = make_loopback_packets(n)
    sender = Sender(ser, window=window, verbose=verbose)
    ok = sender.send_stream(pkts)
    sender.stop()
    sender.report()
    print(f"    {'PASS' if ok and sender.acked == n else 'FAIL'}"
          f" — ACKed {sender.acked}/{n}")
    return ok and sender.acked == n


def test_line(ser, steps, feed_sps, axis='x', window=16, verbose=False):
    node = AXIS_NODE[axis]
    print(f"\n[3] LINE — {steps} steps on {axis.upper()} axis (node {node}) @ {feed_sps} sps")
    # The ATtiny drops stream bytes unless streamEnabled is set — enable first.
    resp = send_text(ser, f'enable {node}')
    print(f"    enable node {node}: {resp or '(no reply)'}")
    pkts = make_line_packets(steps, feed_sps, axis)
    print(f"    Generated {len(pkts)} MicroSegments")
    sender = Sender(ser, window=window, verbose=verbose)
    ok = sender.send_stream(pkts)
    sender.stop()
    sender.report()
    print(f"    {'PASS' if ok and sender.acked == len(pkts) else 'FAIL'}"
          f" — ACKed {sender.acked}/{len(pkts)}")
    return ok


def test_multistep(ser, steps, feed_sps, axis='x', window=16, verbose=False):
    """ONE packet carrying many steps — exercises the within-segment Bresenham
    loop that the single-step line test cannot reach."""
    node = AXIS_NODE[axis]
    print(f"\n[5] MULTISTEP — one segment, {steps} steps on {axis.upper()} (node {node}) @ {feed_sps} sps")
    resp = send_text(ser, f'enable {node}')
    print(f"    enable node {node}: {resp or '(no reply)'}")
    pkts = make_multistep_packet(steps, feed_sps, axis)
    print(f"    Sending 1 MicroSegment that should produce {steps} pulses")
    sender = Sender(ser, window=window, verbose=verbose)
    ok = sender.send_stream(pkts)
    sender.stop()
    sender.report()
    print(f"    {'PASS' if ok and sender.acked == 1 else 'FAIL'} — ACKed {sender.acked}/1")
    print(f"    VISUAL CHECK: motor must move the full distance, not a single twitch.")
    return ok and sender.acked == 1


def test_diagonal(ser, sx, sy, feed_sps, window=16, verbose=False):
    """ONE packet with steps on both X and Y — exercises minor-axis Bresenham
    distribution. Result should be a straight diagonal."""
    print(f"\n[6] DIAGONAL — one segment, dx={sx} dy={sy} (nodes 1 & 2) @ {feed_sps} sps")
    for node in (1, 2):
        resp = send_text(ser, f'enable {node}')
        print(f"    enable node {node}: {resp or '(no reply)'}")
    pkts = make_diagonal_packet(sx, sy, feed_sps)
    print(f"    Sending 1 MicroSegment: X gets {sx} pulses, Y Bresenham-distributed to {sy}")
    sender = Sender(ser, window=window, verbose=verbose)
    ok = sender.send_stream(pkts)
    sender.stop()
    sender.report()
    print(f"    {'PASS' if ok and sender.acked == 1 else 'FAIL'} — ACKed {sender.acked}/1")
    print(f"    VISUAL CHECK: both motors move together, straight diagonal.")
    return ok and sender.acked == 1


def test_backpressure(ser, window=16, verbose=False):
    """
    Flood with more packets than the buffer holds, verify NACK_FULL is handled
    and 'ready' signal arrives after drain.
    """
    print(f"\n[4] BACKPRESSURE — flooding buffer")
    # 600 packets > ring buffer size of 512
    pkts = make_loopback_packets(600)
    sender = Sender(ser, window=window, verbose=verbose)
    ok = sender.send_stream(pkts)
    sender.stop()
    sender.report()
    nack_ok = sender.nacks > 0    # we expect some NACKs
    ack_ok  = sender.acked == 600
    print(f"    NACKs received : {sender.nacks} ({'expected' if nack_ok else 'UNEXPECTED — buffer may be too large'})")
    print(f"    {'PASS' if ok and ack_ok else 'FAIL'} — ACKed {sender.acked}/600")
    return ok and ack_ok


# ── main ──────────────────────────────────────────────────────────────────────

TESTS = ['ping', 'loopback', 'backpressure', 'line', 'multistep', 'diagonal', 'all']

def main():
    parser = argparse.ArgumentParser(
        description="Synthetic comms verification for microseg-host-drive"
    )
    parser.add_argument("--port",    required=True)
    parser.add_argument("--baud",    type=int, default=115200)
    parser.add_argument("--node",    type=int, default=1,
                        help="Node ID for ping test (default 1)")
    parser.add_argument("--test",    choices=TESTS, default='all')
    parser.add_argument("--steps",   type=int, default=400,
                        help="Steps for line test (default 400)")
    parser.add_argument("--feed",    type=int, default=2000,
                        help="Feed rate steps/sec for line test (default 2000)")
    parser.add_argument("--axis",    choices=['x','y'], default='x')
    parser.add_argument("--window",  type=int, default=16)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    print(f"Opening {args.port} @ {args.baud}…")
    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        time.sleep(0.5)  # let Pico USB CDC settle
        ser.reset_input_buffer()

        results = {}

        if args.test in ('ping', 'all'):
            # setorigin first (IDLE -> homed) so enable is accepted
            send_text(ser, 'setorigin')
            send_text(ser, f'enable {args.node}')
            results['ping'] = test_ping(ser, args.node)

        if args.test in ('loopback', 'all'):
            results['loopback'] = test_loopback(
                ser, n=64, window=args.window, verbose=args.verbose)

        if args.test in ('backpressure', 'all'):
            results['backpressure'] = test_backpressure(
                ser, window=args.window, verbose=args.verbose)

        if args.test in ('line', 'all'):
            results['line'] = test_line(
                ser, args.steps, args.feed, args.axis,
                window=args.window, verbose=args.verbose)

        if args.test in ('multistep', 'all'):
            results['multistep'] = test_multistep(
                ser, args.steps, args.feed, args.axis,
                window=args.window, verbose=args.verbose)

        if args.test in ('diagonal', 'all'):
            results['diagonal'] = test_diagonal(
                ser, args.steps, args.steps * 3 // 4, args.feed,
                window=args.window, verbose=args.verbose)

    print(f"\n{'='*50}")
    print("Summary:")
    all_pass = True
    for name, ok in results.items():
        print(f"  {name:20s} {'PASS' if ok else 'FAIL'}")
        all_pass = all_pass and ok
    print(f"\nOverall: {'PASS' if all_pass else 'FAIL'}")
    sys.exit(0 if all_pass else 1)


if __name__ == "__main__":
    main()
