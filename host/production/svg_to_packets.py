"""
svg_to_packets.py — SVG → binary MicroSegment wire packets (host production mode)

Full pipeline runs on the host PC (stages 1-3 then the per-sample look-ahead:
flatten -> constrain -> plan -> discretize -> serialise). The Pico receives
pre-computed step events and emits them directly without any onboard planning.

Usage:
  python svg_to_packets.py input.svg                  # pipe to stdout
  python svg_to_packets.py input.svg --out file.bin   # write to file
  python svg_to_packets.py input.svg --summary        # print stats, no output

On Windows stdout is opened in text mode by default which corrupts binary data.
This script always writes to sys.stdout.buffer (binary mode).
"""

import sys, argparse, struct

from pipeline.stages.stage2 import load_svg_mm_subpaths
from pipeline.stages.stage3 import enforce_c1
from pipeline.stages.flatten import flatten
from pipeline.stages.constrain import constrain
from pipeline.stages.plan_lookahead import plan
from pipeline.stages.discretize import discretize
from host.protocol.packets import serialise_microsegments
from pipeline.stages.config import default as config_default, MachineConfig, KNIFE, PEN


def run(svg_path, machine, feed_max=None, a_max=None, angle_tol=None, gap_tol=None,
        jog_feed=None, quality=None, lift_height=0.0, z_feed=None,
        tangential=True, profile=None):
    """
    Full host pipeline: SVG → MicroSegment packets (the per-sample look-ahead
    pipeline: stages 1-3 then flatten -> constrain -> plan -> discretize).
    Returns list of 26-byte bytes objects.

    quality defaults to config.default().quality.
    feed_max defaults to the tool's profile.feed_max (cut-feed target).
    a_max defaults to machine.x.accel (XY-plane ramp/centripetal accel).
    lift_height > 0 enables Z pen-lift between subpaths.
    profile — the ToolProfile (KNIFE/CREASE/PEN); carries tangent tracking,
    corner threshold, AND the unwind flag (wire protection). When None it is
    selected from `tangential` (KNIFE / PEN). Using the real profile here — not
    the loose tangential bool — is what enables the knife's A unwind in
    production; the bool path built an ad-hoc profile with unwind off.
    """
    if profile is None:
        profile = KNIFE if tangential else PEN

    # Stage 2: SVG → mm subpaths, then the shared subpaths→packets core.
    subpaths_mm, _ = load_svg_mm_subpaths(svg_path)
    return subpaths_to_packets(subpaths_mm, machine, profile,
                               feed_max=feed_max, a_max=a_max,
                               angle_tol=angle_tol, gap_tol=gap_tol,
                               jog_feed=jog_feed, quality=quality,
                               lift_height=lift_height, z_feed=z_feed)


def subpaths_to_packets(subpaths_mm, machine, profile, feed_max=None, a_max=None,
                        angle_tol=None, gap_tol=None, jog_feed=None, quality=None,
                        lift_height=0.0, z_feed=None):
    """
    The tool-aware core: mm subpaths + a ToolProfile → MicroSegment packets
    (Stage 3 repair → flatten → constrain → plan → discretize → serialise). Shared
    by run() (whole-SVG single tool) and the multi-tool planner (one call per
    layer with that layer's tool). feed_max/a_max default to the tool/machine.
    """
    if quality is None:
        quality = config_default().quality
    tangential = profile.tangential
    if feed_max is None:
        feed_max = profile.feed_max
    if a_max is None:
        a_max = machine.x.accel

    repaired = [enforce_c1(sp, angle_tol, gap_tol)[0] for sp in subpaths_mm]

    corner_stop = profile.corner_angle_deg if tangential else None
    a_rate  = machine.a.max_rate if tangential else 0.0
    a_accel = machine.a.accel    if tangential else 0.0

    samples = flatten(repaired, quality=quality)
    constrain(samples, feed_max, a_max, a_rate_deg_s=a_rate,
              a_accel_deg_s2=a_accel, corner_stop_angle_deg=corner_stop)
    plan(samples, machine, a_max=a_max)
    segments = discretize(samples, machine, profile=profile, quality=quality,
                          jog_feed=jog_feed, lift_height=lift_height, z_feed=z_feed)

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
    parser.add_argument("--feed-max",       type=float, default=None,
                        help="Cut feed mm/s (default: tool's profile feed_max)")
    parser.add_argument("--a-max",          type=float, default=None,
                        help="Acceleration mm/s² (default: machine X accel)")
    parser.add_argument("--jog-feed",       type=float, default=None,
                        help="Travel speed between subpaths, mm/s")
    parser.add_argument("--lift-height",    type=float, default=0.0,
                        help="Pen/tool Z lift between subpaths, mm (0 = draw through)")
    parser.add_argument("--z-feed",         type=float, default=None,
                        help="Z raise/lower speed, mm/s")
    parser.add_argument("--tangential",     action=argparse.BooleanOptionalAction,
                        default=True,
                        help="A-axis tangent tracking for knife/crease; "
                             "use --no-tangential for a pen")
    parser.add_argument("--a-accel",        type=float, default=None,
                        help="Override A-axis angular accel, deg/s^2 (gentles "
                             "pivots + bounds in-cut tracking accel; 0 disables)")
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
        spm = args.steps_per_mm if args.steps_per_mm is not None else cfg.machine.x.steps_per_unit
        spd = args.steps_per_deg if args.steps_per_deg is not None else cfg.machine.a.steps_per_unit
        machine = MachineConfig.uniform(spm, spd, args.f_cpu)
    else:
        machine = cfg.machine

    # A-accel override (hardware sweep): replace only machine.a.accel.
    if args.a_accel is not None:
        from dataclasses import replace
        machine = replace(machine, a=replace(machine.a, accel=args.a_accel))

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
