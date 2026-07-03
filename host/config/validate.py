"""
host/config/validate.py — sanity checks for a resolved PipelineConfig.

Catches values that are structurally valid dataclass instances but physically
nonsensical (zero/negative calibration, colliding bus addresses, degenerate
algorithm tuning) before the pipeline runs on them. Returns every problem
found rather than raising on the first, so a bad TOML file or job-override
set gets one complete report instead of a fix-rerun-fix loop.
"""

from pipeline.config import PipelineConfig


def validate(cfg: PipelineConfig) -> list:
    errors = []
    m = cfg.machine

    axes = {"x": m.x, "y": m.y, "z": m.z, "a": m.a}
    node_owner = {}
    for ltr, axis in axes.items():
        if axis.steps_per_unit <= 0:
            errors.append(f"machine.{ltr}: steps_per_unit must be positive (got {axis.steps_per_unit})")
        if axis.max_rate < 0:
            errors.append(f"machine.{ltr}: max_rate cannot be negative (got {axis.max_rate})")
        if axis.accel < 0:
            errors.append(f"machine.{ltr}: accel cannot be negative (got {axis.accel})")
        if axis.max_travel < 0:
            errors.append(f"machine.{ltr}: max_travel cannot be negative (got {axis.max_travel})")
        nid = axis.node.node_id
        if nid in node_owner:
            errors.append(f"duplicate node_id {nid}: used by both {node_owner[nid]} and machine.{ltr}")
        else:
            node_owner[nid] = f"machine.{ltr}"

    for p in m.peripherals:
        if p.node_id in node_owner:
            errors.append(f"duplicate node_id {p.node_id}: used by both {node_owner[p.node_id]} and peripheral '{p.role}'")
        else:
            node_owner[p.node_id] = f"peripheral '{p.role}'"

    if m.f_cpu <= 0:
        errors.append(f"machine.f_cpu must be positive (got {m.f_cpu})")
    if m.jog_feed < 0:
        errors.append(f"machine.jog_feed cannot be negative (got {m.jog_feed})")
    if m.z_feed < 0:
        errors.append(f"machine.z_feed cannot be negative (got {m.z_feed})")

    # Every tool in the job's registry, not just the one mounted on the head
    # -- a multi-tool job (pen + knife layers in one SVG) resolves each
    # layer against tool_profiles, so a bad preset for an unmounted tool
    # would otherwise go unvalidated until it's actually used mid-job.
    for profile in cfg.tool_profiles.values():
        if profile.feed_max <= 0:
            errors.append(f"tool '{profile.name}': feed_max must be positive (got {profile.feed_max})")
        if profile.jog_feed < 0:
            errors.append(f"tool '{profile.name}': jog_feed cannot be negative (got {profile.jog_feed})")
        if profile.z_feed < 0:
            errors.append(f"tool '{profile.name}': z_feed cannot be negative (got {profile.z_feed})")
        if profile.accel < 0:
            errors.append(f"tool '{profile.name}': accel cannot be negative (got {profile.accel})")
        if profile.needs_offset_comp:
            errors.append(
                f"tool '{profile.name}': offset_mm={profile.offset_mm} exceeds the uncompensated "
                f"tolerance and offset compensation is not implemented yet"
            )

    q = cfg.quality
    if q.chord_tol <= 0:
        errors.append(f"quality.chord_tol must be positive (got {q.chord_tol})")
    if q.junction_deviation <= 0:
        errors.append(f"quality.junction_deviation must be positive (got {q.junction_deviation})")
    if q.ds_max <= 0:
        errors.append(f"quality.ds_max must be positive (got {q.ds_max})")
    if q.v_min <= 0:
        errors.append(f"quality.v_min must be positive (got {q.v_min})")
    if q.n_kappa <= 0:
        errors.append(f"quality.n_kappa must be positive (got {q.n_kappa})")

    return errors
