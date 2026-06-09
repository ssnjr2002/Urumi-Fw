"""
svg_to_packets.py — SVG → binary MicroSegment wire packets (host production mode)

Full pipeline: stages 1–6 run on the host PC. The Pico receives pre-computed
step events and emits them directly without any onboard planning.

Usage:
  python svg_to_packets.py input.svg                  # pipe to stdout
  python svg_to_packets.py input.svg --out file.bin   # write to file
  python svg_to_packets.py input.svg --summary        # print stats, no output

On Windows stdout is opened in text mode by default which corrupts binary data.
This script always writes to sys.stdout.buffer (binary mode).
"""

import sys, os, argparse, struct

_PIPELINE = os.path.join(os.path.dirname(__file__), "..", "pipeline", "stages")
sys.path.insert(0, _PIPELINE)

_HOST = os.path.dirname(__file__)
sys.path.insert(0, _HOST)

from stage2 import load_svg_mm_subpaths
from stage3 import enforce_c1
from stage4 import compute_metrics
from stage5 import plan_velocities, PATH_START, PATH_END, MERGE_WITH_PREV
from stage6 import evaluate_microsegments
from serialise import serialise_microsegments

try:
    from pipeline.data.mock_stage6 import MachineConfig
except ImportError:
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pipeline", "data"))
    from mock_stage6 import MachineConfig


def _build_flags(subpaths):
    """Build per-curve flags list across all subpaths."""
    flags = []
    for subpath in subpaths:
        for i in range(len(subpath)):
            f = 0
            if i == 0:            f |= PATH_START
            if i == len(subpath) - 1: f |= PATH_END
            flags.append(f)
    return flags


def run(svg_path, machine, feed_max, a_max, angle_tol, gap_tol, jog_feed=None):
    """
    Full host pipeline: SVG → MicroSegment packets.
    Returns list of 26-byte bytes objects.
    """
    # Stage 2: SVG → mm subpaths
    subpaths_mm, _ = load_svg_mm_subpaths(svg_path)

    # Stage 3: C1 continuity repair
    repaired = [enforce_c1(sp, angle_tol, gap_tol)[0] for sp in subpaths_mm]

    # Flatten for stages 4-5, keeping flags aligned
    flat_curves = [c for sp in repaired for c in sp]
    flags = _build_flags(repaired)

    # Stage 4: metrics (arc length + curvature)
    metrics = compute_metrics(flat_curves)

    # Stage 5: velocity planning
    planned = plan_velocities(metrics, flags, feed_max, a_max)

    # Stage 6: Bezier → MicroSegments (jog_feed defaults to stage6.JOG_FEED)
    if jog_feed is None:
        segments = evaluate_microsegments(planned, machine)
    else:
        segments = evaluate_microsegments(planned, machine, jog_feed=jog_feed)

    # Serialise to wire packets
    return list(serialise_microsegments(segments))


def write_stream(packets, dest):
    """Write length-prefixed framing: [uint16 LE packet_len][packet_bytes]"""
    for pkt in packets:
        dest.write(struct.pack("<H", len(pkt)))
        dest.write(pkt)
    dest.flush()


def main():
    parser = argparse.ArgumentParser(
        description="SVG → binary MicroSegment packet stream (host production)"
    )
    parser.add_argument("svg",              help="Input SVG file")
    parser.add_argument("--out",            help="Write to file instead of stdout")
    parser.add_argument("--summary",        action="store_true",
                        help="Print stats to stderr only, no binary output")
    parser.add_argument("--feed-max",       type=float, default=80.0,
                        help="Max feed rate mm/s (default 80)")
    parser.add_argument("--a-max",          type=float, default=1000.0,
                        help="Acceleration mm/s² (default 1000)")
    parser.add_argument("--jog-feed",       type=float, default=None,
                        help="Travel speed between subpaths, mm/s (default 80)")
    parser.add_argument("--steps-per-mm",   type=float, default=80.0,
                        help="Steps per mm XY (default 80)")
    parser.add_argument("--steps-per-deg",  type=float, default=10.0,
                        help="Steps per degree A axis (default 10)")
    parser.add_argument("--f-cpu",          type=int,   default=150_000_000,
                        help="RP2350 CPU frequency Hz (default 150000000)")
    parser.add_argument("--angle-tol",      type=float, default=5.0,
                        help="C1 angle tolerance degrees (default 5.0)")
    parser.add_argument("--gap-tol",        type=float, default=0.01,
                        help="Gap tolerance mm (default 0.01)")
    args = parser.parse_args()

    machine = MachineConfig(args.steps_per_mm, args.steps_per_deg, args.f_cpu)

    packets = run(args.svg, machine, args.feed_max, args.a_max,
                  args.angle_tol, args.gap_tol, jog_feed=args.jog_feed)

    total_bytes = sum(len(p) for p in packets)
    print(f"MicroSegments : {len(packets)}", file=sys.stderr)
    print(f"Total bytes   : {total_bytes}", file=sys.stderr)

    if args.summary:
        return

    if args.out:
        with open(args.out, "wb") as f:
            write_stream(packets, f)
        print(f"Wrote → {args.out}", file=sys.stderr)
    else:
        out = sys.stdout.buffer if hasattr(sys.stdout, "buffer") else sys.stdout
        write_stream(packets, out)


if __name__ == "__main__":
    main()
