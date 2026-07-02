"""
bake.py — SVG → .plan file (Phase 1 offline bake).

Runs the full pipeline (stages 1-9) on the host PC per layer and serialises the
result into a self-describing .plan binary. The GUI loads the .plan and streams
it; the expensive pipeline only runs once, at bake time.

Layer names in the SVG drive tool selection automatically: a "knife" layer cuts
with KNIFE, "crease" with CREASE, "pen" with PEN (case-insensitive). Pass
--tool to set a fallback for unlayered SVGs or unrecognised layer names.

Usage:
  python -m host.production.bake design.svg
  python -m host.production.bake design.svg --output cut.plan
  python -m host.production.bake design.svg --tool pen          # unlayered SVG
"""

import argparse, os, sys

from pipeline.stages.config import default as _config_default, TOOL_PROFILES
from host.production.planner import plan_job
from host.plan_io import save_plan


def main():
    ap = argparse.ArgumentParser(description="Bake an SVG to a .plan file")
    ap.add_argument("svg", help="Input SVG file")
    ap.add_argument("--output", "-o", default=None,
                    help="Output .plan path (default: <svg-basename>.plan)")
    ap.add_argument("--tool", default=None, choices=list(TOOL_PROFILES),
                    help="Default tool for unlayered SVGs or unrecognised layer names")
    args = ap.parse_args()

    out = args.output or os.path.splitext(args.svg)[0] + ".plan"
    machine = _config_default().machine
    default_tool = TOOL_PROFILES[args.tool] if args.tool else None

    print(f"baking  {args.svg}")
    try:
        plan = plan_job(args.svg, machine, default_tool=default_tool)
    except Exception as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

    ok, problems = plan.feasible_on(machine)
    if not ok:
        for tool, reason in problems:
            print(f"  feasibility fail  {tool}: {reason}", file=sys.stderr)
        sys.exit(1)

    print(f"  {len(plan.operations)} operation(s):")
    for i, op in enumerate(plan.operations):
        print(f"    {i + 1}. {op.tool:<8}  {len(op.packets):>5} segments")

    save_plan(plan, out)
    size = os.path.getsize(out)
    print(f"saved   {out}  ({size:,} bytes)")


if __name__ == "__main__":
    main()
