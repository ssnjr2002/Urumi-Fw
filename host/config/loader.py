"""
host/config/loader.py — TOML -> PipelineConfig, layered on pipeline.config.default().

A TOML file only needs to specify what differs from the code defaults; any
field/section it omits falls through to pipeline.config.default() untouched,
so there is exactly one baseline (pipeline.config.default()) and TOML is
strictly an override on top of it -- never a second independent definition.

Only a single active tool head is supported (matches MachineConfig's current
"single centred head" scope -- dual-head TOML support is a follow-up once the
pipeline actually consumes a second head).

Section layout:

    [machine]
    f_cpu = 150000000
    jog_feed = 80.0
    z_feed = 20.0

    [machine.x]        # AxisConfig fields + node_id/role/present for its BusNode
    node_id = 1
    steps_per_unit = 160.0
    max_rate = 80.0
    accel = 1000.0
    invert = true

    [machine.y]
    ...

    [machine.head]      # the active ToolHead
    tool = "knife"       # name into the (possibly [tools.*]-patched) presets
    x_offset = 0.0

    [machine.head.z]     # AxisConfig fields for the head's Z axis
    ...
    [machine.head.a]     # AxisConfig fields for the head's A axis
    ...

    [[peripherals]]      # non-axis BusNodes; repeatable
    node_id = 5
    role = "oscillator"
    present = false

    [tools.knife]         # patches the KNIFE preset before [machine.head] resolves it
    feed_max = 60.0
    accel = 1800.0

    [quality]
    chord_tol = 0.01
    junction_deviation = 0.05
"""

import tomllib
from dataclasses import replace

import pipeline.config as pcfg
from pipeline.config import (
    AxisConfig, BusNode, MachineConfig, PipelineConfig, QualityConfig,
    ToolHead, ToolProfile,
)
from host.config.validate import validate

_AXIS_FIELDS = ("steps_per_unit", "max_rate", "accel", "max_travel", "invert", "rotary")
_NODE_FIELDS = ("node_id", "role", "present")
_TOOL_PROFILE_FIELDS = (
    "tangential", "offset_mm", "unwind", "corner_angle_deg", "min_radius_mm",
    "feed_max", "accel", "lift_height", "z_feed", "jog_feed",
)
_MACHINE_SCALAR_FIELDS = ("f_cpu", "jog_feed", "z_feed")
_QUALITY_FIELDS = (
    "chord_tol", "dv_max", "v_min", "dt_max", "dt_min", "angle_tol", "gap_tol",
    "n_kappa", "junction_deviation", "ds_max", "dtheta_max",
)


def _load_axis(base: AxisConfig, d: dict) -> AxisConfig:
    node_kwargs = {k: d[k] for k in _NODE_FIELDS if k in d}
    node = replace(base.node, **node_kwargs) if node_kwargs else base.node
    axis_kwargs = {k: d[k] for k in _AXIS_FIELDS if k in d}
    return replace(base, node=node, **axis_kwargs)


def _load_tool_profile(base: ToolProfile, d: dict) -> ToolProfile:
    kwargs = {k: d[k] for k in _TOOL_PROFILE_FIELDS if k in d}
    return replace(base, **kwargs) if kwargs else base


def _load_head(base: ToolHead, d: dict, tool_profiles: dict) -> ToolHead:
    z = _load_axis(base.z, d["z"]) if "z" in d else base.z
    a = _load_axis(base.a, d["a"]) if "a" in d else base.a
    profile = base.profile
    if "tool" in d:
        name = d["tool"]
        if name not in tool_profiles:
            raise ValueError(f"[machine.head] tool = '{name}' is not a known tool preset")
        profile = tool_profiles[name]
    x_offset = d.get("x_offset", base.x_offset)
    return replace(base, z=z, a=a, profile=profile, x_offset=x_offset)


def _load_peripherals(entries: list) -> tuple:
    return tuple(
        BusNode(node_id=p["node_id"], role=p.get("role", "stepper"), present=p.get("present", True))
        for p in entries
    )


def _load_machine(base: MachineConfig, d: dict, tool_profiles: dict) -> MachineConfig:
    x = _load_axis(base.x, d["x"]) if "x" in d else base.x
    y = _load_axis(base.y, d["y"]) if "y" in d else base.y
    active = base.heads[base.active_head]
    head = _load_head(active, d.get("head", {}), tool_profiles) if "head" in d else active
    scalars = {k: d[k] for k in _MACHINE_SCALAR_FIELDS if k in d}
    return replace(base, x=x, y=y, heads=(head,) + base.heads[1:], **scalars)


def _load_quality(base: QualityConfig, d: dict) -> QualityConfig:
    kwargs = {k: d[k] for k in _QUALITY_FIELDS if k in d}
    return replace(base, **kwargs) if kwargs else base


def load(path) -> PipelineConfig:
    """
    Parse a TOML file and merge it onto pipeline.config.default(), then
    validate the result. Raises ValueError (with every problem found, not
    just the first) if the merged config is invalid.
    """
    base = pcfg.default()
    with open(path, "rb") as f:
        data = tomllib.load(f)

    tool_profiles = dict(pcfg.TOOL_PROFILES)
    for name, overrides in data.get("tools", {}).items():
        if name not in tool_profiles:
            raise ValueError(f"[tools.{name}] does not match any known tool preset")
        tool_profiles[name] = _load_tool_profile(tool_profiles[name], overrides)

    machine = _load_machine(base.machine, data.get("machine", {}), tool_profiles)
    if "peripherals" in data:
        machine = replace(machine, peripherals=_load_peripherals(data["peripherals"]))
    quality = _load_quality(base.quality, data.get("quality", {}))

    cfg = replace(base, machine=machine, quality=quality)
    errors = validate(cfg)
    if errors:
        raise ValueError("config validation failed:\n  " + "\n  ".join(errors))
    return cfg
