"""
planner.py — SVG layers → an ordered tool-tagged plan (multi-tool 8b).

The plan is a sequence of ToolOperations, each a run of motion for one tool.
It carries NO MSEG_FLAG_PAUSE — tool changes are just "the next op uses a
different tool"; the sender resolves them to pauses (single head) or head
switches (dual head) at stream time (see docs/PLAN_phase1_host_impl.md §11).

Today's planner is trivial: one operation per SVG layer, in document order, with
the layer's tool (tool_for_layer). The operation ABSTRACTION is what matters —
a smarter planner (interleaving for travel/required order) can replace this
function without changing the plan format or anything below it.
"""

from dataclasses import dataclass, field

from pipeline.stages.stage2 import load_svg_mm_layers
from pipeline.stages.config import default as config_default, tool_for_layer, can_run_tool
from host.production.svg_to_packets import subpaths_to_packets


@dataclass
class ToolOperation:
    tool:    str           # ToolProfile.name
    profile: object        # the ToolProfile
    packets: list          # list[bytes] — MSEG step packets for this op


@dataclass
class Plan:
    operations: list = field(default_factory=list)   # in execution order

    @property
    def tools(self):
        """Unique tools the plan uses, in first-appearance order."""
        seen = []
        for op in self.operations:
            if op.tool not in seen:
                seen.append(op.tool)
        return seen

    def feasible_on(self, machine):
        """
        (ok, problems) — can `machine`'s topology run every tool this plan uses?
        The upfront, config-only gate (no hardware). problems is a list of
        (tool, reason) for the tools that don't fit.
        """
        problems = []
        for op in self.operations:
            ok, reason = can_run_tool(machine, op.profile)
            if not ok and (op.tool, reason) not in problems:
                problems.append((op.tool, reason))
        return (not problems), problems


def plan_job(svg_path, machine, overrides=None, default_tool=None, quality=None,
             lift_height=0.0):
    """
    SVG → Plan. One ToolOperation per layer, in document order. A layer whose
    name resolves to a tool (tool_for_layer / overrides) cuts with it; an
    unresolved layer falls back to `default_tool` if given, else raises (so a
    mislabelled layer is caught, not silently dropped).

    An unlayered SVG yields a single '' layer — pass default_tool to run it as a
    one-tool job, or just use svg_to_packets.run for the single-tool path.
    """
    if quality is None:
        quality = config_default().quality
    layers_mm, _ = load_svg_mm_layers(svg_path)

    ops = []
    for name, subpaths in layers_mm.items():
        profile = tool_for_layer(name, overrides) or default_tool
        if profile is None:
            raise ValueError(
                f"layer {name!r} has no tool — rename it to a tool "
                f"(pen/knife/crease), pass overrides, or set default_tool")
        packets = subpaths_to_packets(subpaths, machine, profile,
                                      quality=quality, lift_height=lift_height)
        ops.append(ToolOperation(tool=profile.name, profile=profile, packets=packets))
    return Plan(operations=ops)
