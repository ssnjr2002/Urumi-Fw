"""
verify_packets.py — offline validator and trajectory visualiser for MicroSegment streams

Usage:
  python svg_to_packets.py input.svg --out job.bin
  python verify_packets.py --in job.bin
  python verify_packets.py --in job.bin --plot
  python svg_to_packets.py input.svg | python verify_packets.py

Reads a length-prefixed binary stream (written by svg_to_packets.py), validates
every packet (magic, size, CRC), and optionally plots the reconstructed XY
trajectory by accumulating dx/dy step deltas.
"""

import sys, os, argparse, struct

from host.protocol.packets import (
    validate_packet, unpack_microsegment,
    MAGIC_MICROSEG, MSEG_FLAG_PATH_END,
)


# ── framing reader ─────────────────────────────────────────────────────────────

def read_packets(src):
    """Read length-prefixed packets written by svg_to_packets.write_stream."""
    while True:
        header = src.read(2)
        if not header:
            break
        if len(header) < 2:
            print("WARN: truncated length header", file=sys.stderr)
            break
        (length,) = struct.unpack("<H", header)
        data = src.read(length)
        if len(data) < length:
            print(f"WARN: expected {length}B, got {len(data)}B", file=sys.stderr)
            break
        yield data


# ── stats ──────────────────────────────────────────────────────────────────────

class Stats:
    def __init__(self):
        self.total      = 0
        self.ok         = 0
        self.crc_fail   = 0
        self.bad_magic  = 0
        self.bad_size   = 0
        self.segments   = []   # list of unpacked dicts

    def report(self):
        print(f"\n{'-'*50}")
        print(f"Packets total  : {self.total}")
        print(f"  OK           : {self.ok}")
        print(f"  CRC failures : {self.crc_fail}")
        print(f"  Bad magic    : {self.bad_magic}")
        print(f"  Bad size     : {self.bad_size}")
        if self.segments:
            total_x = sum(s['dx'] for s in self.segments)
            total_y = sum(s['dy'] for s in self.segments)
            total_a = sum(s['da'] for s in self.segments)
            ivs = [s['interval'] for s in self.segments]
            print(f"Net steps      : dx={total_x}  dy={total_y}  da={total_a}")
            print(f"Interval range : {min(ivs)} – {max(ivs)} cycles")
        ok = self.crc_fail == 0 and self.bad_magic == 0 and self.bad_size == 0
        print(f"\nResult         : {'PASS' if ok else 'FAIL'}")
        return ok


# ── verification ───────────────────────────────────────────────────────────────

def verify_stream(src, verbose=False):
    stats = Stats()

    for raw in read_packets(src):
        stats.total += 1

        ok, reason = validate_packet(raw)
        if not ok:
            if 'magic' in reason:   stats.bad_magic += 1
            elif 'size' in reason:  stats.bad_size  += 1
            elif 'CRC'  in reason:  stats.crc_fail  += 1
            else:                   stats.bad_magic  += 1
            print(f"  [{stats.total:5d}] FAIL — {reason}", file=sys.stderr)
            continue

        ms = unpack_microsegment(raw)
        stats.segments.append(ms)
        stats.ok += 1

        if verbose:
            flag_str = '|'.join(f for f, b in [
                ('PATH_END', ms['flags'] & MSEG_FLAG_PATH_END),
            ] if b) or '-'
            print(f"  [{stats.total:5d}]  dx={ms['dx']:6d}  dy={ms['dy']:6d}"
                  f"  dz={ms['dz']:5d}  da={ms['da']:5d}"
                  f"  iv={ms['interval']:10d}  flags={flag_str}")

    return stats


# ── trajectory plot ────────────────────────────────────────────────────────────

def plot_trajectory(segments, steps_per_mm=80.0):
    try:
        import matplotlib.pyplot as plt
        import numpy as np
    except ImportError:
        print("matplotlib not installed — skipping plot", file=sys.stderr)
        return

    # Reconstruct XY position by accumulating dx/dy
    xs, ys = [0.0], [0.0]
    x = y = 0.0
    path_breaks = []   # indices where PATH_END occurs

    for i, ms in enumerate(segments):
        x += ms['dx'] / steps_per_mm
        y += ms['dy'] / steps_per_mm
        xs.append(x)
        ys.append(y)
        if ms['flags'] & MSEG_FLAG_PATH_END:
            path_breaks.append(len(xs) - 1)

    xs = np.array(xs)
    ys = np.array(ys)

    fig, ax = plt.subplots(figsize=(8, 8))
    ax.set_aspect('equal')
    ax.set_title(f"Reconstructed trajectory  ({len(segments)} MicroSegments)")
    ax.set_xlabel("X (mm)")
    ax.set_ylabel("Y (mm)")

    # Split into subpaths at PATH_END boundaries and plot each
    prev = 0
    color_cycle = plt.rcParams['axes.prop_cycle'].by_key()['color']
    for ci, end_idx in enumerate(path_breaks + [len(xs) - 1]):
        sl = slice(prev, end_idx + 1)
        ax.plot(xs[sl], ys[sl], color=color_cycle[ci % len(color_cycle)], lw=0.8)
        prev = end_idx

    ax.invert_yaxis()  # match SVG Y-down convention
    plt.tight_layout()
    plt.show()


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Validate and visualise a MicroSegment binary stream"
    )
    parser.add_argument("--in",          dest="infile",
                        help="Read from file instead of stdin")
    parser.add_argument("--plot",        action="store_true",
                        help="Plot reconstructed XY trajectory")
    parser.add_argument("--steps-per-mm", type=float, default=80.0,
                        help="Steps per mm for trajectory plot (default 80)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.infile:
        src = open(args.infile, "rb")
    else:
        src = sys.stdin.buffer if hasattr(sys.stdin, "buffer") else sys.stdin

    stats = verify_stream(src, verbose=args.verbose)
    ok = stats.report()

    if args.infile:
        src.close()

    if args.plot and stats.segments:
        plot_trajectory(stats.segments, args.steps_per_mm)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
