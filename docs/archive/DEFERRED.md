# Deferred Work

**Branch:** `phase1-impl`  
**Date:** 2026-06-29  

---

## 1. Dual-head sender

**What:** The `send_plan` sender in `host/job_runner.py` is single-head only.
It resolves every tool change as a physical swap (PAUSE → operator mounts next
tool → resume). A dual-head machine can switch tools without a physical swap by
changing which head is streaming — no pause, no operator action, just a
host-side head offset change.

**Prompted by:** The multi-tool job model design (§11 of
`PLAN_phase1_host_impl.md`). The model was designed with dual-head in mind
(one plan, any machine), but the sender was implemented single-head first in
step 8b/8c because dual-head has many combos to reason about:

- Are both tools already mounted at job start, or does one require a swap?
- If three tools are needed on a two-head machine, one swap is still required —
  dual-head advantage is bounded by head count, not unlimited.
- Pre-flight for dual-head is more complex: which tools are currently mounted on
  which heads determines whether a swap happens at all.

**Where to implement:** `host/job_runner.py` — add a `send_plan_dual_head`
variant (or extend `send_plan` with a `dual_head=True` flag) that routes tool
changes through `select_head` rather than `operator.mount`. The `Plan` format
and `.plan` file are already machine-agnostic and require no changes.

---

## 2. Tool orchestration as an explicit pipeline stage

**What:** The current `plan_job` (in `host/production/planner.py`) treats
SVG layer order as the final execution order — one operation per layer, in
document order. This is correct for Phase 1 but not adequate for a real
multi-tool job: you may want to interleave subpaths across layers to minimise
total travel, eliminate unnecessary tool changes, or respect a cut-before-crease
constraint.

**Prompted by:** "Depending on the job, the planner could plan the job to use
the tools in the optimal way possible… Sometimes it might require intertwining
tools." Confirmed today when articulating the distinction between per-tool
choreography and cross-tool orchestration (see §4 below).

**Where to implement:** A dedicated orchestration stage that sits in the data
flow **between stage 6 (Plan) and stage 7+8 (Discretize)**. Its input is
planned samples tagged by layer/tool; its output is an ordered list of
`(tool, [subpaths])` ready to hand to Discretize. For Phase 1 the stage is a
pass-through (layer order). A real implementation would do travel-optimal
reordering, grouping, or interleaving.

**Prerequisite:** See §3 — the pipeline must be splittable at the 6/7 boundary
before this stage can exist.

---

## 3. Pipeline split at the stage 6 / stage 7+8 boundary

**What:** `subpaths_to_packets` (and the functions it calls) currently runs
stages 1–9 in one shot per layer. For a tool orchestration stage (§2) to work,
it needs to be possible to run stages 1–6 independently for all layers, hand
the resulting planned samples to the orchestrator, and then run stages 7+8+9
per `(tool, subpath)` in the orchestrated order.

**Prompted by:** The orchestration stage discussion. Without this split, the
orchestrator has no handle on the intermediate representation — it can only
reorder whole layers of already-discretized packets, which loses the
choreography context (lift positions, corner flags, etc.).

**Where to implement:** Expose two independent entry points in the production
pipeline: `plan_subpaths(subpaths_mm, machine, quality)` → planned samples, and
`discretize_subpaths(planned_samples, machine, profile)` → packets. The current
`subpaths_to_packets` becomes their composition. `plan_job` would then call
`plan_subpaths` per layer, pass results to the orchestrator, then call
`discretize_subpaths` per orchestrated operation.

---

## 4. Choreography vs. orchestration distinction

**What:** Two different concerns that the pipeline must address, at different
stages:

- **Choreography** (per-tool) — the motion detail *within* a single tool's
  operation: Z-lift between subpaths, A pre-orientation at path start,
  lift-pivot-lower at sharp corners, wired-tool A unwind. Handled by Discretize
  (stage 7+8) via `ToolProfile`. Already implemented; each tool's `ToolProfile`
  drives different choreography transparently.

- **Orchestration** (cross-tool) — the order and grouping of *operations across
  tools*: should the knife cuts in region A come before or after the crease
  lines? Can two knife subpaths bracket a crease subpath to avoid a redundant
  tool change? This is the planner's freedom and is **not** expressible inside
  Discretize — it must happen before Discretize, at the planned-sample level.

**Prompted by:** "We have to be concerned about the choreography required by
each tool and then also about orchestration for all tools." The two were
previously conflated; this session separated them and identified that they live
at different stages. Choreography is already solved; orchestration is
deliberately trivial in Phase 1 (layer order) and deferred.

---

## 5. Peripheral controller nodes in pre-flight

**What:** `KNIFE.required_peripheral_roles` is currently an empty tuple. When a
physical oscillating-knife controller node exists on the bus, the role
`"oscillator"` should be added to KNIFE's profile and the corresponding
`BusNode` (with that role) added to `machine.peripherals`. Pre-flight will then
automatically ping it as part of the per-activation physical check.

**Prompted by:** "If the tool has a controller like the oscillating knife, we
need to make sure the controller is present and ready." The `required_peripheral_roles`
field and the pre-flight resolution logic are fully implemented; the hardware
node and its role declaration are simply not real yet. No code changes required
when the node arrives — just config additions.

**Also:** Adding a new tool type that doesn't map to PEN/KNIFE/CREASE requires
extending the `ToolType` enum in `config.py` and bumping `VERSION` in
`host/plan_io.py` (old `.plan` files with the old version byte will be
rejected cleanly). Prompted by the `.plan` format design discussion — this is
the known coupling point.

---

## 6. Host-computed auto-return to pausePos

**What:** After a tool-change PAUSE, the operator may jog the head to a
convenient swap position. Before `resume` is sent, the host must compute and
stream jog packets that return the head to the pause position. This is not yet
implemented: `send_plan` currently calls `resume` immediately after the
per-activation pre-flight, without returning to any saved position.

**Prompted by:** §4 of `PLAN_phase1_host_impl.md` ("auto-return to pausePos")
and §7 (Phase 1 vs. Phase 2 split). The design is fully specified: jog during
PAUSED lands back in PAUSED (via `runningReason=JOG` + `active=true`); only
after the return jog drains does the host send `resume`. The missing piece is
the host-side delta computation (current position from `getpos` minus the saved
pausePos) and the return jog packet generation.

**Dependency:** Requires the Pico firmware track (steps 2–5 in the impl order
table) to expose `getpos` and honour `runningReason=JOG` + PAUSED-return
semantics. Not exercisable until hardware integration (step 10).

---

## 7. Live bus overview

**What:** A live per-node status layer — `NodeStatus` (present / enabled /
homed / position + driver faults) fetched via a future `getbus` query, surfaced
as a GUI Bus panel. Pre-flight then becomes a thin filter over live bus state
rather than axis-mask polling.

**Prompted by:** "What we really need is a bus overview — it's the same
epiphany we had when we refactored the config.py." The bus-first config model
(§10 of `PLAN_phase1_host_impl.md`) is already in place; the live dynamic layer
is deferred to firmware time when the Pico can enumerate nodes and cache their
health. For now: presence via `pingnode` / ping-all, gating via `getstate`
`enabled`/`homed` masks.
