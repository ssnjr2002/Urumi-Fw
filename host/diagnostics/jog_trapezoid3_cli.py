"""
jog_trapezoid3_cli.py — hand-built 3-packet trapezoid (accel, cruise, decel),
one packet per phase, for jog blending attempt 2.

jog_x2_cli.py showed the wire/buffer layer is NOT the source of the felt
"pause": two independent make_jog() bursts ACK cleanly with zero NACKs/retries.
The dip comes from make_jog() itself always anchoring each independent call's
first and last segment at v0 (default 50 sps) -- so two consecutive bursts
always slow to a crawl and speed back up at the boundary, even though the wire
never actually stalls.

This script isolates the boundary question one level down: build ONE logical
X +10mm move as exactly 3 MicroSegment/JOG packets --

    packet 1: accel phase, one packet, interval set for the phase's average
              velocity (v0 -> feed)
    packet 2: cruise phase, one packet, constant feed velocity
    packet 3: decel phase, one packet, average velocity (feed -> v0)

-- with velocity continuous across the three packets by construction (no
independent trapezoids, no per-call v0 reset). Use --split to choose whether
they're sent as one continuous burst (baseline: should be smooth) or as three
separate stream() sessions back-to-back (tests whether a session boundary
itself -- independent of velocity discontinuity -- introduces a felt pause).

Usage:
  python -m host.diagnostics.jog_trapezoid3_cli --port COM8
  python -m host.diagnostics.jog_trapezoid3_cli --port COM8 --split each
"""

import argparse
import math
import time
from collections import namedtuple

from host.protocol.link import Link
from host.protocol.session import ListSource
from host.protocol.packets import pack_jog, MSEG_FLAG_NONE, MSEG_FLAG_PATH_END
from pipeline.config import default as _config_default

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])


def _send_burst(link, packets, label, window=16, verbose=True):
    link.reset_seq()
    sess = link.session(ListSource(packets), window=window, verbose=verbose)
    t0 = time.monotonic()
    ok = sess.run()
    t1 = time.monotonic()
    print(f"[{label}] ok={ok} time={t1 - t0:.3f}s sent={sess.sent} "
          f"acked={sess.acked} nacks={sess.nacks} retries={sess.retries}")
    return ok


def main():
    ap = argparse.ArgumentParser(description="Hand-built 3-packet accel/cruise/decel X jog")
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--split", choices=["none", "each"], default="none",
                    help="'none' = one stream() call for all 3 packets (baseline); "
                         "'each' = one stream() call per packet, back-to-back")
    args = ap.parse_args()

    machine = _config_default().machine
    ax = machine.x

    dist, rate, sign = 20.0, 80.0, 1
    accel = max(rate * 8.0, 50.0)
    v0 = 50.0

    feed_sps = rate * ax.steps_per_unit
    accel_sps2 = accel * ax.steps_per_unit
    total_steps = abs(int(dist * sign * ax.steps_per_unit))
    invert = -1 if ax.invert else 1

    d_acc = (feed_sps**2 - v0**2) / (2.0 * accel_sps2)
    if 2 * d_acc > total_steps:
        # Can't reach feed_sps in the available distance -- symmetric triangle
        # profile instead (no cruise phase).
        peak = math.sqrt(v0**2 + accel_sps2 * total_steps)
        d_acc = (peak**2 - v0**2) / (2.0 * accel_sps2)
        feed_sps = peak
    d_acc = int(round(d_acc))
    d_dec = d_acc
    d_cruise = total_steps - d_acc - d_dec

    v_acc_avg  = (v0 + feed_sps) / 2.0
    v_dec_avg  = (feed_sps + v0) / 2.0
    interval_acc  = max(1, min(int(machine.f_cpu / v_acc_avg), machine.f_cpu))
    interval_crz  = max(1, min(int(machine.f_cpu / feed_sps), machine.f_cpu))
    interval_dec  = max(1, min(int(machine.f_cpu / v_dec_avg), machine.f_cpu))

    print(f"total_steps={total_steps} d_acc={d_acc} d_cruise={d_cruise} d_dec={d_dec} "
          f"feed_sps={feed_sps:.0f} v_acc_avg={v_acc_avg:.0f} v_dec_avg={v_dec_avg:.0f}")

    def seg(steps, interval, last):
        dx = invert * sign * steps
        flags = MSEG_FLAG_PATH_END if last else MSEG_FLAG_NONE
        return pack_jog(_MS(dx=dx, dy=0, dz=0, da=0, interval=interval, flags=flags))

    packets = []
    if d_acc > 0:
        packets.append(seg(d_acc, interval_acc, last=(d_cruise == 0 and d_dec == 0)))
    if d_cruise > 0:
        packets.append(seg(d_cruise, interval_crz, last=(d_dec == 0)))
    print(f"{d_dec = }")
    if d_dec > 0:
        packets.append(seg(d_dec, interval_dec, last=True))

    print(f"Built {len(packets)} hand-built packets, split={args.split}")

    link = Link.open_serial(args.port, baud=args.baud)
    try:
        time.sleep(0.3)
        link.backend.serial.reset_input_buffer()
        print("enable:", link.command(f"enable {ax.node.node_id}"))
        print("getstate (before):", link.command("getstate"))

        if args.split == "none":
            _send_burst(link, packets, "trapezoid (single burst)")
        else:
            labels = ["accel", "cruise", "decel"]
            for p, label in zip(packets, labels):
                _send_burst(link, [p], label)

        time.sleep(0.5)
        print("getstate (after):", link.command("getstate"))
    finally:
        link.close()


if __name__ == "__main__":
    main()
