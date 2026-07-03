"""
bake.py — SVG → .plan file (Phase 1 offline bake).

Runs the full pipeline (stages 1-9) on the host PC per layer and serialises the
result into a self-describing .plan binary. The GUI loads the .plan and streams
it; the expensive pipeline only runs once, at bake time.

Layer names in the SVG drive tool selection automatically: a "knife" layer cuts
with KNIFE, "crease" with CREASE, "pen" with PEN (case-insensitive). Pass
--tool to set a fallback for unlayered SVGs or unrecognised layer names.

Blocks run in document order by default. Pass --tool-order to group and
reorder blocks by tool instead (document order preserved within each tool's
group) — e.g. run every knife block before any crease block regardless of
how the layers were interleaved in the SVG.

Usage:
  python -m host.production.bake design.svg
  python -m host.production.bake design.svg --output cut.plan
  python -m host.production.bake design.svg --tool pen          # unlayered SVG
  python -m host.production.bake design.svg --tool-order knife,crease
  python -m host.production.bake design.svg --config machine.toml
  python -m host.production.bake design.svg --feed-max 40 --accel 900
"""

import argparse, os, sys

from pipeline.config import default as _config_default, TOOL_PROFILES
from host.production.planner import plan_job
from host.production.plan_io import save_plan
import host.config as host_config


def main():
    ap = argparse.ArgumentParser(description="Bake an SVG to a .plan file")
    ap.add_argument("svg", help="Input SVG file")
    ap.add_argument("--output", "-o", default=None,
                    help="Output .plan path (default: <svg-basename>.plan)")
    ap.add_argument("--config", default=None,
                    help="TOML config (host.config.load); default: pipeline.config.default()")
    ap.add_argument("--tool", default=None, choices=list(TOOL_PROFILES),
                    help="Default tool for unlayered SVGs or unrecognised layer names")
    ap.add_argument("--tool-order", default=None,
                    help="Comma-separated tool names, e.g. knife,crease — groups "
                         "and reorders blocks by tool (default: document order)")
    ap.add_argument("--feed-max", type=float, default=None,
                    help="Per-job override: programmed cut feed for --tool (mm/s)")
    ap.add_argument("--accel", type=float, default=None,
                    help="Per-job override: programmed accel target for --tool (units/s^2)")
    args = ap.parse_args()

    out = args.output or os.path.splitext(args.svg)[0] + ".plan"

    if args.config:
        cfg = host_config.load(args.config)
    else:
        cfg = _config_default()
        errors = host_config.validate(cfg)
        if errors:
            print("error: config validation failed:\n  " + "\n  ".join(errors), file=sys.stderr)
            sys.exit(1)

    if args.feed_max is not None or args.accel is not None:
        if not args.tool:
            print("error: --feed-max/--accel require --tool (job overrides are per-tool)", file=sys.stderr)
            sys.exit(1)
        override = {k: v for k, v in (("feed_max", args.feed_max), ("accel", args.accel)) if v is not None}
        try:
            cfg = host_config.apply_tool_overrides(cfg, {args.tool: override})
        except ValueError as e:
            print(f"error: {e}", file=sys.stderr)
            sys.exit(1)

    machine = cfg.machine
    # cfg.tool_profiles carries every TOML/job-override patch, keyed by tool
    # name -- passed as plan_job's per-layer overrides so a pen layer picks
    # up a [tools.pen] patch even when knife is the tool mounted on the head.
    default_tool = cfg.tool_profiles.get(args.tool) if args.tool else None
    tool_order = args.tool_order.split(",") if args.tool_order else None

    print(f"baking  {args.svg}")
    try:
        plan = plan_job(args.svg, machine, overrides=cfg.tool_profiles,
                         default_tool=default_tool, quality=cfg.quality, tool_order=tool_order)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    # feasibility check (Plan.feasible_on) intentionally not run here yet —
    # deferred until the feasibility-gate pass lands.

    print(f"  {len(plan.operations)} operation(s):")
    for i, op in enumerate(plan.operations):
        print(f"    {i + 1}. {op.tool:<8}  {len(op.packets):>5} segments")

    save_plan(plan, out)
    size = os.path.getsize(out)
    print(f"saved   {out}  ({size:,} bytes)")


if __name__ == "__main__":
    main()
