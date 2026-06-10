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
from config import default as config_default, MachineConfig


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


def run(svg_path, machine, feed_max, a_max, angle_tol, gap_tol,
        jog_feed=None, quality=None, lift_height=0.0, z_feed=None,
        tangential=True):
    """
    Full host pipeline: SVG → MicroSegment packets.
    Returns list of 26-byte bytes objects.
    quality defaults to config.default().quality.
    lift_height > 0 enables Z pen-lift between subpaths.
    tangential — A-axis tangent tracking (knife/crease). Off for a pen.
    """
    if quality is None:
        quality = config_default().quality
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

    # Stage 6: Bezier → MicroSegments (jog_feed/z_feed default to config motion tier)
    segments = evaluate_microsegments(planned, machine,
                                      quality=quality, jog_feed=jog_feed,
                                      lift_height=lift_height, z_feed=z_feed,
                                      tangential=tangential)

    # Serialise to wire packets
    return list(serialise_microsegments(segments))


def write_stream(packets, dest):
    """Write length-prefixed framing: [uint16 LE packet_len][packet_bytes]"""
    for pkt in packets:
        dest.write(struct.pack("<H", len(pkt)))
        dest.write(pkt)
    dest.flush()


def main():
    cfg = config_default()
    parser = argparse.ArgumentParser(
        description="SVG → binary MicroSegment packet stream (host production)"
    )
    parser.add_argument("svg",              help="Input SVG file")
    parser.add_argument("--out",            help="Write to file instead of stdout")
    parser.add_argument("--summary",        action="store_true",
                        help="Print stats to stderr only, no binary output")
    parser.add_argument("--feed-max",       type=float, default=cfg.motion.feed_max,
                        help="Max feed rate mm/s")
    parser.add_argument("--a-max",          type=float, default=cfg.motion.a_max,
                        help="Acceleration mm/s²")
    parser.add_argument("--jog-feed",       type=float, default=None,
                        help="Travel speed between subpaths, mm/s")
    parser.add_argument("--lift-height",    type=float, default=cfg.motion.lift_height,
                        help="Pen/tool Z lift between subpaths, mm (0 = draw through)")
    parser.add_argument("--z-feed",         type=float, default=None,
                        help="Z raise/lower speed, mm/s")
    parser.add_argument("--tangential",     action=argparse.BooleanOptionalAction,
                        default=True,
                        help="A-axis tangent tracking for knife/crease; "
                             "use --no-tangential for a pen")
    parser.add_argument("--steps-per-mm",   type=float, default=None,
                        help="Override XY steps/mm (default: real per-axis config)")
    parser.add_argument("--steps-per-deg",  type=float, default=None,
                        help="Override A steps/deg (default: real per-axis config)")
    parser.add_argument("--f-cpu",          type=int,   default=cfg.machine.f_cpu,
                        help="RP2350 CPU frequency Hz")
    parser.add_argument("--angle-tol",      type=float, default=cfg.quality.angle_tol,
                        help="C1 angle tolerance degrees")
    parser.add_argument("--gap-tol",        type=float, default=cfg.quality.gap_tol,
                        help="Gap tolerance mm")
    args = parser.parse_args()

    # Default to the real per-axis machine (honours Z=1200, A=120, etc.).
    # Scalar flags force a uniform machine only when explicitly given.
    if args.steps_per_mm is not None or args.steps_per_deg is not None:
        spm = args.steps_per_mm if args.steps_per_mm is not None else cfg.machine.steps_per_mm
        spd = args.steps_per_deg if args.steps_per_deg is not None else cfg.machine.steps_per_deg
        machine = MachineConfig.uniform(spm, spd, args.f_cpu)
    else:
        machine = cfg.machine

    packets = run(args.svg, machine, args.feed_max, args.a_max,
                  args.angle_tol, args.gap_tol, jog_feed=args.jog_feed,
                  lift_height=args.lift_height, z_feed=args.z_feed,
                  tangential=args.tangential)

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
