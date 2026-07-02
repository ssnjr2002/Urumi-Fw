"""
planner.py — SVG -> Plan (multi-tool 8b).

plan_job is a library function, not a CLI: it calls orchestrate_layers (the
ordering decision — host.production.orchestrate) then runs the tool-aware
pipeline compile (subpaths_to_packets) per block, in the order
orchestrate_layers picked, and returns the in-memory Plan. Ordering logic
lives entirely in orchestrate.py; a smarter orchestrator (interleaving,
travel optimisation) can replace it without this file changing.

The Plan/ToolOperation types themselves, and .plan file save/load, live in
plan_io.py — bake.py is the CLI that calls plan_job here and then
plan_io.save_plan to write the result to disk.
"""

from host.production.normalise import load_svg_mm_layers
from host.production.orchestrate import orchestrate_layers
from host.production.plan_io import Plan, ToolOperation
from pipeline.stages.config import default as config_default
from host.production.svg_to_packets import subpaths_to_packets


def plan_job(svg_path, machine, overrides=None, default_tool=None, quality=None,
             lift_height=0.0, tool_order=None):
    """
    SVG → Plan. Orchestrates layers into ordered blocks (orchestrate_layers,
    document order by default, or grouped/reordered by `tool_order` — a list of
    tool names) then compiles each block through the tool-aware pipeline core.

    An unlayered SVG yields a single '' layer — pass default_tool to run it as a
    one-tool job, or just use svg_to_packets.run for the single-tool path.
    """
    if quality is None:
        quality = config_default().quality
    layers_mm, _ = load_svg_mm_layers(svg_path)
    blocks = orchestrate_layers(layers_mm, tool_order, overrides, default_tool)

    ops = []
    for name, profile, subpaths in blocks:
        packets = subpaths_to_packets(subpaths, machine, profile,
                                      quality=quality, lift_height=lift_height)
        ops.append(ToolOperation(tool=profile.name, profile=profile, packets=packets))
    return Plan(operations=ops)
