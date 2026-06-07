"""
svg_to_packets.py — SVG → binary RS485 packet stream

Usage:
  python svg_to_packets.py input.svg                  # pipe to stdout
  python svg_to_packets.py input.svg | verify_packets.py
  python svg_to_packets.py input.svg --out file.bin   # write to file instead

On Windows stdout is opened in text mode by default which corrupts binary data.
This script always writes to sys.stdout.buffer (binary mode).
"""

import sys, os, argparse, struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pipeline", "stages"))

from stage2 import load_svg_mm_subpaths
from stage3 import enforce_c1
from stage7 import serialise_paths, TOOL_CONFIG_DEFAULT


def run(svg_path, angle_tol, gap_tol, tool_config):
    subpaths_mm, _ = load_svg_mm_subpaths(svg_path)
    repaired = [enforce_c1(sp, angle_tol, gap_tol)[0] for sp in subpaths_mm]
    return list(serialise_paths(repaired, tool_config=tool_config))


def write_stream(packets, dest):
    """Write length-prefixed framing: [uint16 LE packet_len][packet_bytes]"""
    for pkt in packets:
        dest.write(struct.pack("<H", len(pkt)))
        dest.write(pkt)
    dest.flush()


def main():
    parser = argparse.ArgumentParser(
        description="SVG → binary RS485 packet stream"
    )
    parser.add_argument("svg",         help="Input SVG file")
    parser.add_argument("--out",       help="Write to file instead of stdout")
    parser.add_argument("--angle-tol", type=float, default=5.0,
                        help="C1 angle tolerance in degrees (default 5.0)")
    parser.add_argument("--gap-tol",   type=float, default=0.01,
                        help="Gap tolerance in mm (default 0.01)")
    args = parser.parse_args()

    packets = run(args.svg, args.angle_tol, args.gap_tol, TOOL_CONFIG_DEFAULT)

    if args.out:
        with open(args.out, "wb") as f:
            write_stream(packets, f)
        total = sum(len(p) for p in packets)
        print(f"Wrote {len(packets)} packets ({total} bytes) → {args.out}",
              file=sys.stderr)
    else:
        out = sys.stdout.buffer if hasattr(sys.stdout, "buffer") else sys.stdout
        write_stream(packets, out)


if __name__ == "__main__":
    main()
