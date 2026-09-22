import argparse
import time
from collections import namedtuple

from host.protocol.link import Link
from host.protocol.packets import make_jog, unpack_microsegment, pack_microsegment
from pipeline.config import default as _config_default

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])

def _retag_as_mseg(packets):
    """Rebuild each packet with MAGIC_MICROSEG instead of MAGIC_JOG."""
    out = []
    for p in packets:
        f = unpack_microsegment(p)
        out.append(pack_microsegment(_MS(dx=f["dx"], dy=f["dy"], dz=f["dz"], da=f["da"],
                                          interval=f["interval"], flags=f["flags"])))
    return out

def generate_back_and_forth(args, machine):
    axes_str = args.axes.replace(",", "").lower()
    primary_ax = getattr(machine, axes_str[0])
    
    if args.accel_pct is not None:
        p = args.accel_pct / 100.0
        if p <= 0 or p > 0.5:
            raise ValueError("--accel-pct must be > 0 and <= 50")
        accel = (args.rate ** 2) * (1 - p) / (p * args.dist)
    else:
        accel = max(args.rate * 8.0, 50.0)
        
    feed_sps = args.rate * primary_ax.steps_per_unit
    accel_sps2 = accel * primary_ax.steps_per_unit
    
    vec_forward = [0, 0, 0, 0]
    vec_backward = [0, 0, 0, 0]
    
    for ax_char in axes_str:
        if ax_char not in ['x', 'y', 'z', 'a']:
            continue
        ax = getattr(machine, ax_char)
        dist_steps = int(args.dist * ax.steps_per_unit) * (-1 if ax.invert else 1)
        axis_idx = {"x": 0, "y": 1, "z": 2, "a": 3}[ax_char]
        vec_forward[axis_idx] = dist_steps
        vec_backward[axis_idx] = -dist_steps
    
    packets_forward = make_jog(tuple(vec_forward), feed_sps, accel_sps2, machine.f_cpu)
    packets_backward = make_jog(tuple(vec_backward), feed_sps, accel_sps2, machine.f_cpu)
    
    # Retag MAGIC_JOG to MAGIC_MICROSEG
    mseg_forward = _retag_as_mseg(packets_forward)
    mseg_backward = _retag_as_mseg(packets_backward)
    
    all_packets = []
    for _ in range(args.n):
        all_packets.extend(mseg_forward)
        all_packets.extend(mseg_backward)
        
    return all_packets

def main():
    ap = argparse.ArgumentParser(description="Stream microsegment packets to send an axis back and forth n times")
    ap.add_argument("--port", required=True, help="Serial port to connect to")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    ap.add_argument("--axes", default="x", help="Axes to move, e.g. x, xy, xyz (default: x)")
    ap.add_argument("--dist", type=float, default=10.0, help="Distance in mm to move back and forth (default: 10.0)")
    ap.add_argument("--rate", type=float, default=80.0, help="Feed rate in mm/s (default: 80.0)")
    ap.add_argument("--n", type=int, default=5, help="Number of times to go back and forth (default: 5)")
    ap.add_argument("--accel-pct", type=float, default=None, help="Percentage of total travel time spent accelerating (0 to 50). If omitted, uses default heuristic.")
    args = ap.parse_args()

    machine = _config_default().machine

    packets = generate_back_and_forth(args, machine)
    
    print(f"Built {len(packets)} packets for {args.axes.upper()} +/-{args.dist}mm @ {args.rate}mm/s for {args.n} cycles")
    
    link = Link.open_serial(args.port, baud=args.baud)
    try:
        time.sleep(0.3)
        link.backend.serial.reset_input_buffer()
        
        print("unalarm:", link.command("unalarm"))
        
        axes_str = args.axes.replace(",", "").lower()
        for ax_char in axes_str:
            if ax_char in ['x', 'y', 'z', 'a']:
                ax = getattr(machine, ax_char)
                print(f"enable {ax.node.node_id}:", link.command(f"enable {ax.node.node_id}"))

        t0 = time.monotonic()
        ok = link.stream(packets, verbose=True)
        t1 = time.monotonic()
        print(f"stream() returned {ok} in {t1 - t0:.3f}s")
        
        time.sleep(0.5)
        print("getstate:", link.command("getstate"))
    except KeyboardInterrupt:
        print("\nCtrl-C detected! Sending estop and disable all...")
        print("stop:", link.command("stop"))
        print("axes_enable off:", link.command("axes_enable off"))
        print("getstate:", link.command("getstate"))
    finally:
        link.close()

if __name__ == "__main__":
    main()