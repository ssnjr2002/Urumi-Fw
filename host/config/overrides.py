"""
host/config/overrides.py — per-job, per-tool patch applied on top of a
loaded/validated PipelineConfig, right before .plan generation.

This is the third tier: default() -> load(toml) -> apply_tool_overrides().
The UI/CLI only ever patches the tool(s) actually used in the job being
built; anything unspecified keeps whatever load()/default() already
resolved for that tool. Runs once at build time -- the pipeline stages never
see job_overrides, only the PipelineConfig that comes out the other end.

Patches cfg.tool_profiles (the full per-name registry), not just
machine.head.profile: a job can use more than one tool in one SVG (a pen
layer and a knife layer), and orchestrate_layers/tool_for_layer resolves
each LAYER's tool against tool_profiles, not against whichever tool happens
to be physically mounted on the head. machine.head.profile is also patched
when its name matches, purely so machine-level feasibility checks
(can_run_tool, select_head) stay consistent with the same override.
"""

from dataclasses import replace

from pipeline.config import PipelineConfig
from host.config.loader import _TOOL_PROFILE_FIELDS
from host.config.validate import validate


def apply_tool_overrides(cfg: PipelineConfig, job_overrides: dict) -> PipelineConfig:
    """
    job_overrides: {tool_name: {field: value, ...}, ...}, e.g.
        {"knife": {"feed_max": 60.0, "jog_feed": 90.0}, "pen": {"feed_max": 40.0}}

    Only feed_max/accel/jog_feed/z_feed (and the other ToolProfile fields
    load() already accepts) are patchable, and only for tool names present in
    cfg.tool_profiles. Raises ValueError (with every problem found) if the
    result doesn't validate, or if job_overrides names an unknown tool or an
    unknown field.
    """
    if not job_overrides:
        return cfg

    tool_profiles = dict(cfg.tool_profiles)
    for tool_name, override in job_overrides.items():
        if tool_name not in tool_profiles:
            raise ValueError(f"job override names unknown tool '{tool_name}'")
        unknown = set(override) - set(_TOOL_PROFILE_FIELDS)
        if unknown:
            raise ValueError(f"tool '{tool_name}': unknown override field(s): {sorted(unknown)}")
        tool_profiles[tool_name] = replace(tool_profiles[tool_name], **override)

    machine = cfg.machine
    head = machine.head
    if head.profile.name in job_overrides:
        patched_head = replace(head, profile=tool_profiles[head.profile.name])
        machine = replace(machine, heads=(patched_head,) + machine.heads[1:])

    patched_cfg = replace(cfg, machine=machine, tool_profiles=tool_profiles)

    errors = validate(patched_cfg)
    if errors:
        raise ValueError("config validation failed after job overrides:\n  " + "\n  ".join(errors))
    return patched_cfg
