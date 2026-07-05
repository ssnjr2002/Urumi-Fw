"""
jog_x2_cli.py — crude prototype for jog blending attempt 2.

Question being tested: if two jog bursts are sent as two SEPARATE stream()
calls (two separate Sender/Go-Back-N sessions, each with its own internal
seqreset) with ZERO deliberate gap between them — does the Pico's ring buffer
stay non-empty across the boundary, so Core 1 never falls back to STATE_IDLE
and the second burst's JOG packets are accepted without a NACK_BAD_STATE?

Requires the e27abf2 firmware fix (JOG accepted when
state==RUNNING && runningReason==RUNNING_JOG) to be REFLASHED onto the Pico
first — on the old firmware this will just reproduce the known bug (second
burst's first packet(s) rejected while state==RUNNING from burst 1), which
doesn't tell us anything new about the buffer-drain theory.

Hardcoded: X +10mm @ 80mm/s, twice back-to-back, no blending math at all —
just two independent make_jog() bursts fired one after another as fast as
Python can call stream() again.

Usage:
  python host/diagnostics/jog_x2_cli.py --port COM8
"""

import argparse
import time

from host.protocol.link import Link
from host.protocol.stream import Sender
from host.protocol.packets import make_jog
from pipeline.config import default as _config_default


def _send_burst(link, packets, label, window=16, verbose=True):
    sender = Sender(link.serial, window=window, verbose=verbose)
    try:
        t0 = time.monotonic()
        ok = sender.send_stream(packets)
        t1 = time.monotonic()
    finally:
        sender.stop()
    print(f"[{label}] ok={ok} time={t1 - t0:.3f}s sent={sender.sent} "
          f"acked={sender.acked} nacks={sender.nacks} retries={sender.retries}")
    return ok


def main():
    ap = argparse.ArgumentParser(description="Two back-to-back X+10mm@80mm/s jog bursts, zero gap")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    machine = _config_default().machine
    ax = machine.x

    dist, rate, sign = 10.0, 80.0, 1
    accel = max(rate * 8.0, 50.0)
    feed_sps = rate * ax.steps_per_unit
    accel_sps2 = accel * ax.steps_per_unit
    dist_steps = int(dist * sign * ax.steps_per_unit) * (-1 if ax.invert else 1)

    packets_a = make_jog((dist_steps, 0, 0, 0), feed_sps, accel_sps2, machine.f_cpu)
    packets_b = make_jog((dist_steps, 0, 0, 0), feed_sps, accel_sps2, machine.f_cpu)
    print(f"Built {len(packets_a)} + {len(packets_b)} packets for two X +{dist}mm @ {rate}mm/s bursts "
          f"(steps={dist_steps}, node={ax.node.node_id})")

    link = Link.open_serial(args.port, baud=args.baud)
    try:
        time.sleep(0.3)
        link.backend.serial.reset_input_buffer()
        print("enable:", link.command(f"enable {ax.node.node_id}"))

        print("getstate (before):", link.command("getstate"))

        ok_a = _send_burst(link, packets_a, "burst A")
        # No sleep here on purpose -- this is the zero-gap case under test.
        ok_b = _send_burst(link, packets_b, "burst B")

        time.sleep(0.5)
        print("getstate (after):", link.command("getstate"))

        if ok_a and ok_b:
            print("Both bursts ACKed cleanly with zero deliberate gap.")
        else:
            print("At least one burst failed -- see GO-BACK/NACK lines above.")
    finally:
        link.close()


if __name__ == "__main__":
    main()
