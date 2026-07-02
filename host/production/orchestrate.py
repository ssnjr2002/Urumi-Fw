"""
orchestrate.py — SVG layers -> an ordered, tool-tagged block list.

Pure ordering decision: no pipeline calls, no packet compilation. Takes the
layer/subpath geometry ingest produces (host.production.normalise) and decides
which block runs when. Phase 1 orchestration is trivial (one block per layer,
document order, or reordered by an explicit tool_order) — see
docs/multi_tool_orchestration_strategy.md for where this grows (travel
optimisation, interleaving, cut-before-crease constraints) without touching
planner.py's compile step below it.
"""

from pipeline.stages.config import tool_for_layer


def orchestrate_layers(layers_mm, tool_order=None, overrides=None, default_tool=None):
    """
    layers_mm — ordered {layer_name: list[subpath]} from load_svg_mm_layers.
    tool_order — optional list of tool names; blocks are grouped and reordered
    to match this order (stable within each tool — document order preserved
    inside a group). None (default) keeps document order as-is.
    overrides/default_tool — passed through to tool_for_layer resolution,
    same semantics as plan_job previously had inline.

    Returns list[(layer_name, profile, subpaths)] in execution order. Raises
    ValueError for a layer whose name resolves to no tool (mislabelled layer,
    caught here rather than silently dropped downstream).
    """
    blocks = []
    for name, subpaths in layers_mm.items():
        profile = tool_for_layer(name, overrides) or default_tool
        if profile is None:
            raise ValueError(
                f"layer {name!r} has no tool — rename it to a tool "
                f"(pen/knife/crease), pass overrides, or set default_tool")
        blocks.append((name, profile, subpaths))

    if tool_order:
        rank = {tool: i for i, tool in enumerate(tool_order)}
        blocks.sort(key=lambda b: rank.get(b[1].name, len(rank)))

    return blocks
