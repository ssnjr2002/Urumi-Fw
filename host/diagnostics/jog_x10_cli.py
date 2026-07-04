"""
jog_x10_cli.py — minimal, single-threaded harness around the new UI's own
jog backend (make_jog + Link), with none of the UI's background threads:
no poll worker, no jog queue/worker thread, no Tkinter. Isolates whether the
mid-burst stall seen via DEBUG_TIMING's texp/tmeas/twall is caused by the
UI's background thread contention (poll_worker racing the jog stream) or
something deeper in the host protocol/link/firmware layer.

Hardcoded: X +10mm @ 80mm/s, sent once, then prints `getstate` (which includes
texp/tmeas/twall if the firmware was built with -DDEBUG_TIMING).

Usage:
  python host/diagnostics/jog_x10_cli.py --port COM8
"""

import argparse
import time
from collections import namedtuple

from host.protocol.link import Link
from host.protocol.packets import make_jog, unpack_microsegment, pack_microsegment
from pipeline.config import default as _config_default

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])


def _retag_as_mseg(packets):
    """Rebuild each packet with MAGIC_MICROSEG instead of MAGIC_JOG, same fields
    and CRC recomputed -- a cheap host-only A/B test of whether the firmware's
    JOG_MAGIC state-gate (IDLE/PAUSED only) is what's rejecting packets 2+ of a
    burst once Core 1 flips state to RUNNING, without touching/reflashing
    firmware at all."""
    out = []
    for p in packets:
        f = unpack_microsegment(p)
        out.append(pack_microsegment(_MS(dx=f["dx"], dy=f["dy"], dz=f["dz"], da=f["da"],
                                          interval=f["interval"], flags=f["flags"])))
    return out


def main():
    ap = argparse.ArgumentParser(description="Send one hardcoded X+10mm@80mm/s jog, no UI/threads involved")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--magic", choices=["jog", "mseg"], default="jog",
                    help="Packet magic to send: 'jog' (MAGIC_JOG, default) or 'mseg' "
                         "(retag as MAGIC_MICROSEG to test the state-gate theory without reflashing)")
    args = ap.parse_args()

    machine = _config_default().machine
    ax = machine.x

    dist, rate, sign = 10.0, 80.0, 1
    # Same accel formula OnlineSession.jog() uses.
    accel = max(rate * 8.0, 50.0)

    feed_sps = rate * ax.steps_per_unit
    accel_sps2 = accel * ax.steps_per_unit
    dist_steps = int(dist * sign * ax.steps_per_unit) * (-1 if ax.invert else 1)

    vec = (dist_steps, 0, 0, 0)
    packets = make_jog(vec, feed_sps, accel_sps2, machine.f_cpu)
    if args.magic == "mseg":
        packets = _retag_as_mseg(packets)
    print(f"Built {len(packets)} packets ({args.magic}) for X +{dist}mm @ {rate}mm/s "
          f"(steps={dist_steps}, feed_sps={feed_sps:.0f}, accel_sps2={accel_sps2:.0f}, "
          f"node={ax.node.node_id})")

    link = Link.open_serial(args.port, baud=args.baud)
    try:
        time.sleep(0.3)
        link.backend.serial.reset_input_buffer()
        print("enable:", link.command(f"enable {ax.node.node_id}"))
        # No standalone "seqreset" here -- Sender.send_stream() already sends
        # its own internal seqreset at the start of the stream; old jog.py
        # never sends a second one either.

        t0 = time.monotonic()
        ok = link.stream(packets, verbose=True)
        t1 = time.monotonic()
        print(f"stream() returned {ok} in {t1 - t0:.3f}s")

        # No poller running at all during the stream above -- this is the one
        # and only control-plane read, issued after stream() returns.
        time.sleep(0.5)
        print("getstate:", link.command("getstate"))
    finally:
        link.close()


if __name__ == "__main__":
    main()
