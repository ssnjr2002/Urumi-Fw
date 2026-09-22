# Config Schema — TOML Layer (Phase 1, host-side)

**Status:** Phase 1 (host production). Distinct from the Pico-authority
pull/push design in [`PLAN_config_management.md`](PLAN_config_management.md),
which is deferred to Phase 2 (local production on the Pico). This doc covers
the TOML file `host/config/loader.py` reads today — a host-local file, not
something pulled from or pushed to the Pico.

## Three tiers

```
pipeline.config.default()  →  host.config.load(path)  →  host.config.apply_tool_overrides(cfg, job_overrides)
     (code baseline)              (TOML, optional)              (per-job, per-tool patch)
```

1. **`pipeline.config.default()`** — the one hardcoded baseline (`pipeline/config.py`).
   Every stage CLI, test, and TOML load starts here.
2. **`host.config.load(path)`** — parses a TOML file and merges it onto
   `default()` field-by-field via `dataclasses.replace`. Any section or field
   the TOML omits falls through untouched to the code default — there is
   never a second independent definition of a value, only overrides on top
   of the one baseline. Runs `validate()` before returning; raises
   `ValueError` (with every problem found, not just the first) if the merged
   config is invalid.
3. **`host.config.apply_tool_overrides(cfg, job_overrides)`** — a per-job
   patch applied once at `.plan`-build time, after `load()`/`default()` has
   already resolved everything else. `job_overrides` is a plain dict keyed by
   tool name (`{"knife": {"feed_max": 60.0}}`). This is where a UI/CLI feed
   or accel field for the job in progress lands. Re-validates after patching.

Pipeline stages only ever see the final `PipelineConfig` that comes out of
step 3 — they have no idea TOML or job overrides exist.

### `PipelineConfig.tool_profiles` — why a job can patch more than the mounted tool

`PipelineConfig` carries a `tool_profiles: dict[str, ToolProfile]` field —
every tool preset available for the job, not just the one physically mounted
on `machine.head`. This matters because a single SVG can use more than one
tool (a pen layer and a knife layer in the same file): `orchestrate_layers`
resolves each *layer's* tool via `tool_for_layer(name, overrides)`, which
checks `overrides` (conventionally `cfg.tool_profiles`, passed as
`plan_job(..., overrides=cfg.tool_profiles)`) before falling back to the
`pipeline.config.TOOL_PROFILES` code defaults. `machine.head.profile` is
purely topology bookkeeping — which tool is "mounted", used by
`can_run_tool`/`select_head` feasibility checks — not what determines a
layer's actual feed/accel during plan generation.

So both `load()` and `apply_tool_overrides()` patch `cfg.tool_profiles` for
every tool named in `[tools.*]`/`job_overrides`, and only *additionally*
patch `machine.head.profile` when the patched tool happens to be the one
mounted on the head (keeping the two views consistent). Any call site
building a plan (`bake.py`, `host/ui/offline/session.py`'s `generate_plan`)
must pass `overrides=cfg.tool_profiles` to `plan_job` — passing only
`cfg.machine` silently drops every tool-preset patch except the mounted
one's.

## TOML section reference

| Section | Maps to | Fields |
|---|---|---|
| `[machine]` | `MachineConfig` scalars | `f_cpu`, `jog_feed`, `z_feed` |
| `[machine.x]`, `[machine.y]` | `AxisConfig` (+ its `BusNode`) | `node_id`, `role`, `present`, `steps_per_unit`, `max_rate`, `accel`, `max_travel`, `invert`, `rotary` |
| `[machine.head]` | the active `ToolHead` | `tool` (name into the tool-preset table, patched by `[tools.*]` first), `x_offset` |
| `[machine.head.z]`, `[machine.head.a]` | `AxisConfig` for the head's Z/A axes | same `AxisConfig` fields as `[machine.x]` |
| `[[peripherals]]` | one `BusNode` each, repeatable array-of-tables | `node_id`, `role`, `present` |
| `[tools.<name>]` | patches a `ToolProfile` preset (`pen`/`knife`/`crease`) before `[machine.head].tool` resolves it | `tangential`, `offset_mm`, `unwind`, `corner_angle_deg`, `min_radius_mm`, `feed_max`, `accel`, `lift_height`, `z_feed`, `jog_feed` |
| `[quality]` | `QualityConfig` | `chord_tol`, `dv_max`, `v_min`, `dt_max`, `dt_min`, `angle_tol`, `gap_tol`, `n_kappa`, `junction_deviation`, `ds_max`, `dtheta_max` |

Not patchable via TOML or job overrides: `ToolProfile.name`/`tool_type`
(identity, not tuning) and `ToolProfile.required_peripheral_roles`
(topology — declared in code alongside the preset, same reasoning as why
`AxisConfig.node` routing is host-informational only per
`PLAN_config_management.md`'s tier table). `MachineConfig.active_head` and
multi-head (`heads[1:]`) are not yet TOML-configurable — `MachineConfig`
itself only consumes a single active head today; dual-head TOML support is a
follow-up once the pipeline actually consumes a second head.

## Job overrides (not TOML — a runtime dict)

```python
job_overrides = {
    "knife": {"feed_max": 60.0, "accel": 1500.0, "jog_feed": 90.0},
}
cfg = host.config.apply_tool_overrides(cfg, job_overrides)
```

Same field set as `[tools.<name>]` in TOML (`_TOOL_PROFILE_FIELDS` in
`host/config/loader.py` is the single source of truth for what's patchable
at both tier 2 and tier 3 — keeping them in sync is intentional, since a job
override is just a later, per-job version of the same kind of patch a TOML
`[tools.*]` section makes).

## Validation

`host/config/validate.py` runs after every `load()` and every
`apply_tool_overrides()` call. It checks:
- every axis's `steps_per_unit`, `max_rate`, `accel`, `max_travel` is
  non-negative (`steps_per_unit` must be strictly positive)
- no two axes/peripherals share a `node_id`
- `f_cpu`, `jog_feed`, `z_feed` are non-negative
- the active tool's `feed_max` is positive, `accel`/`jog_feed`/`z_feed` are
  non-negative, and `offset_mm` doesn't exceed the uncompensated tolerance
  (`ToolProfile.needs_offset_comp`)
- `QualityConfig`'s `chord_tol`, `junction_deviation`, `ds_max`, `v_min`,
  `n_kappa` are positive

It returns every problem found (not just the first), so a bad file or
override set produces one complete report.

## Example

See [`config_schema_example.toml`](config_schema_example.toml) for a file
that sets every patchable field.
