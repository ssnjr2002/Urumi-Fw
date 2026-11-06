# Archive

Docs from hosts and designs the project has left behind. They are kept for
their reasoning and are not updated: paths, commands and status lines in them
describe code that is gone. Links between them still work; links out of them
may not.

## Python host

The first host, `host/`, a Python pipeline that planned on the PC and streamed
microsegments to the Pico. Replaced by the web host (`web/src`) and the Pico's
own planner and controller.

| Doc | Was | Replaced by |
| --- | --- | --- |
| `DEFERRED.md` | work deferred from the Python host's first phase | the plans in `docs/plans/` |
| `PLAN_microseg_host_drive.md` | driving the machine from host-made microsegments | `docs/plans/pico-planner.md`, `lib/planner` |
| `PLAN_per_axis.md` | per-axis rate limits in the host planner | `docs/plans/pico-planner.md`, `docs/plans/planner-za.md` |
| `PLAN_pipeline_redesign.md` | the host planner's stages | `docs/plans/pico-planner.md` |
| `PLAN_svg_tile_motion.md` | SVG splines cut into tiles of motion | `docs/plans/pico-planner.md`, `web/src/svg`, `web/src/toolpath` |
| `PLAN_phase1_host_impl.md` | host-side job orchestration | `docs/plans/motion-sessions.md`, `docs/plans/job-session-web.md` |
| `PLAN_config_management.md` | config pull and push between host and Pico | `docs/config_storage.md`, `docs/plans/pico-config.md`, `docs/plans/runtime-config.md` |
| `config_management_discussion.md` | the conversation behind the plan above | as above |
| `config_schema.md`, `config_schema_example.toml` | the TOML config | `config/controller.jsonc`, `web/src/machine/schema.ts` |
| `state_redesign.md` | the Pico's states under the Python host | `docs/plans/state-handling.md`, `docs/plans/machine-model.md` |
| `state_config_integration_premortem.md` | risks where state meets config | `docs/plans/state-handling.md` |
| `code_review.md` | a review of the Python UI's observers | nothing |
| `ui_architecture.md`, `online_ui_arch.drawio` | the Python GUI's offline and online tabs | `web/demo` |

## Web planner

The web host before planning and control moved to the Pico: a controller in
`web/src/controller` and a planner ported from TypeScript to C++.

| Doc | Was | Replaced by |
| --- | --- | --- |
| `controller.md` | the controller in `web/src/controller` | the Pico's controller, `docs/plans/machine-model.md` |
| `tool_duty_limits.md` | a host scheduler for a tool's on-time | `docs/plans/job-z.md` |
| `port_workflow.md` | how planner stages were ported to C++ | `lib/planner` |
| `planner_audit.md` | the planner's findings before the C++ port | `lib/planner`, `docs/plans/pico-planner.md` |
| `planner_spaces.md` | which planner operations belong in which space | `lib/planner`, `docs/plans/pico-planner.md` |
| `feed_accel_value_model.md` | the feed and acceleration values in the toolpath pipeline | `web/src/machine/schema.ts` |
| `head_binding.md` | binding a layer's head when the job is baked | Tool change in `docs/plans/motion-sessions.md` |
| `multi_tool_orchestration_strategy.md` | multi-layer, multi-tool jobs split between orchestrator and planner | `docs/plans/motion-sessions.md`, `docs/plans/job-z.md` |

## Pico designs and notes

Notes from the move of planning and control onto the Pico: designs never
built, a store since rewritten, and a checklist from before the Pico ran jobs.

| Doc | Was | Replaced by |
| --- | --- | --- |
| `bus_alarm.md` | a stop started by a node over the bus; never built | nothing |
| `config_storage_littlefs_migration.md` | moving the config blob into LittleFS | `docs/config_storage.md` |
| `coordinate_frames_and_limits.md` | frames and soft limits for the C++ planner | `docs/plans/coordinate-system.md` |
| `feed_override.md` | one feed dial for every unplanned stop; simulated only | nothing |
| `validation_checklist.md` | finding which of host, Pico and nodes a fault is in | nothing yet |
