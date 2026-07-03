"""
host/config/overrides.py — per-job, per-tool patch applied on top of a
loaded/validated PipelineConfig, right before .plan generation.

This is the third tier: default() -> load(toml) -> apply_tool_overrides().
The UI/CLI only ever patches the tool(s) actually used in the job being
built; anything unspecified keeps whatever load()/default() already
resolved for that tool. Runs once at build time -- the pipeline stages never
see job_overrides, only the PipelineConfig that comes out the other end.
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
    load() already accepts) are patchable. Only the active head's tool is
    resolved against job_overrides (single-head scope, same as loader.py).
    Raises ValueError (with every problem found) if the result doesn't
    validate, or if job_overrides names a field ToolProfile doesn't have.
    """
    head = cfg.machine.head
    override = job_overrides.get(head.profile.name)
    if not override:
        return cfg

    unknown = set(override) - set(_TOOL_PROFILE_FIELDS)
    if unknown:
        raise ValueError(f"tool '{head.profile.name}': unknown override field(s): {sorted(unknown)}")

    patched_profile = replace(head.profile, **override)
    patched_head = replace(head, profile=patched_profile)
    machine = cfg.machine
    patched_machine = replace(machine, heads=(patched_head,) + machine.heads[1:])
    patched_cfg = replace(cfg, machine=patched_machine)

    errors = validate(patched_cfg)
    if errors:
        raise ValueError("config validation failed after job overrides:\n  " + "\n  ".join(errors))
    return patched_cfg
