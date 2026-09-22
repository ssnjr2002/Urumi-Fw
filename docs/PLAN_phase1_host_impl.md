# Plan: Phase 1 Host-Side Implementation

**Branch:** `pipeline-redesign`
**Date:** 2026-06-28
**Status:** Design in progress — host orchestration for host-production mode

---

## Scope

Phase 1 = **host production**. The host PC runs the full pipeline (SVG →
microsegments), streams pre-computed step packets to the Pico, and the Pico
executes them without any onboard planning. All kinematics, all motion
profiles, all path computation live on the host.

Hard consequence that shapes everything below: **the Pico cannot generate
motion.** It has no planner, no velocity/accel profile generator. Every move —
including jogs and the auto-return to a paused position — must arrive as
host-computed packets. The Pico only *executes* and *guards*.

This doc covers the host's orchestration: connect → pre-flight → execute →
tool change → resume. The Pico-side state machine it drives is specified in
[state_redesign.md](state_redesign.md); the wire framing is in
[wire_protocol.md](wire_protocol.md). Config (machine/quality/tool) is
host-side only in Phase 1 — see [PLAN_config_management.md](PLAN_config_management.md)
(deferred to Phase 2).

---

## Host Lifecycle Overview

```
connect ──► pre-flight ──► execute job ──► [tool change boundary] ──► resume ──► … ──► done
              │                                      │
              │                                      └─ PAUSE choreography (jog / swap / re-home / return)
              └─ abort with operator message if any check fails
```

---

## 1. Connect

Phase 1 has **no CMD_HANDSHAKE** (that is a Phase 2 config-agreement command).
The host only needs to know the Pico is alive and in a sane state:

- **CMD_PING** the Pico → expects CMD_PONG. Confirms the link.
- **CMD_GET_STATE** (see §6 — protocol gap) → `machineState`. Confirms the Pico
  is in `STATE_IDLE` and ready to take a job (not stuck in ALARM, not mid-job).

No config exchange. The host trusts its own config; the Pico runs firmware
defaults and just executes packets.

---

## 2. Pre-flight Checks

Pre-flight is **entirely host-orchestrated**. The host PC never touches the
RS485 bus directly — it talks to the Pico over USB, and the Pico relays node
pings onto the bus (host → USB → Pico → RS485 → ATtiny). So every machine-state
check below is the host issuing a command to the Pico. The **one exception** is
the operator tool-mounted confirmation (step 6): the Pico cannot sense what tool
is physically mounted, so that gate lives in the host UI, not in any command.

Run **all** checks for the **whole** job before streaming a single packet — a
3-tool job must not discover a missing oscillator controller two hours in.
In order:

1. **Pico alive** — CMD_PING → CMD_PONG.
2. **Pico ready** — CMD_GET_STATE → `machineState == STATE_IDLE`.
3. **All required axis nodes present** — for every head the job uses, ping each
   axis node (`head.z.node`, `head.a.node`, plus the shared `x.node`/`y.node`).
   Confirms every ATtiny driver the job needs is alive on the RS485 bus.
4. **All required peripheral nodes present** — for each tool profile in the job,
   resolve `profile.required_peripheral_roles` against `machine.peripherals`
   (match by role), and ping each resolved node. A tool with no controller
   (e.g. PEN) has an empty tuple → no ping. A knife declares `("oscillator",)`
   → resolve to the bus node, ping it, confirm present and ready.
5. **Axes homed** — CMD_GET_STATE returns `axes_homed`; check
   `(axes_homed & required_axes) == required_axes` for the **first** tool.
   Later tools' axis requirements are re-checked at each tool-change boundary
   (a swap may invalidate position — see §4).
6. **Human confirmation of mounted tools** — the Pico cannot sense what is
   physically mounted. The host knows each head's `profile` from config and
   asks the operator to confirm: "KNIFE on head 0, CREASE on head 1." Phase 1
   has no automated tool-presence sensing; this is a manual gate.

If any check fails, abort before streaming and tell the operator exactly what
is blocking.

---

## 3. Job Execution & Multi-Tool

A job may use multiple tools. Tool groups are planned per tool, and the tool
change boundary becomes a predetermined `MSEG_FLAG_PAUSE` point in the stream
(the host placed it deliberately, so it knows the boundary precisely).

Two machine cases diverge at a tool change:

**Dual-head machine** — a head switch *between the mounted tools* is purely
host-side: the host switches which head's parameters it plans/streams with and
applies that head's `x_offset`. **The Pico never knows a head changed** — it
just keeps executing packets. No physical operator action, no re-home (assuming
both heads were confirmed at pre-flight). A `MSEG_FLAG_PAUSE` may still be used
if the head geometry demands a clean stop.

But N heads only cover N tools. A job needing **more tools than there are
heads** (e.g. 3 tools on a 2-head machine) still requires a *physical swap* for
the surplus tool — the dual-head advantage is bounded by head count, not
unlimited. The swap of that surplus tool follows the single-head choreography
(§4). So a 3-tool job on a 2-head machine = one free host-side head switch +
one physical swap.

**Single-head machine** — the second tool requires a *physical swap*: the job
pauses, the operator changes the tool, and execution resumes. This is the
PAUSE choreography in §4.

> Note (open): `MachineConfig.active_head` is kept as-is for now (a runtime
> field). It is a *runtime execution* concept — which head is live right now —
> **not** a planning variable. Multi-tool planning must NOT mutate config
> mid-plan; instead it resolves the head per tool group (`select_head(machine,
> tool)`) and reads that head's parameters directly. Revisit if/when the
> runtime/plan separation needs to be made explicit in the type.

> Note (open): **SVG layer → tool mapping.** Authoring multi-tool jobs is
> expected to use named SVG layers (`<g>` with `inkscape:label`): a "knife"
> layer → KNIFE profile, a "crease" layer → CREASE profile, etc. Stage 1
> ingests all layers at once; the planner knows which subpaths belong to which
> tool group and places the tool-change `MSEG_FLAG_PAUSE` boundaries. This is a
> Stage 1 ingest concern, captured here so it isn't lost; not yet implemented.

---

## 4. Tool Change / PAUSE Choreography (single head)

The Pico state machine already has the machinery (see state_redesign.md:
PAUSE, jog-during-pause, `isPaused`/`PausedJobContext.active`,
`axes_homed`/`setorigin`). The host drives the operator dialog on top of it.

**Key Phase 1 fact — jog is host-driven, NOT Pico-generated.** Jog packets
(`JOG_MAGIC`) are accepted during `STATE_PAUSED`. The Pico executing a jog is a
transition to `STATE_RUNNING` with `runningReason = JOG` while
`PausedJobContext.active` stays **true**. When the jog queue drains, the `active`
flag is the signal to return to `STATE_PAUSED` (not `STATE_IDLE`). This is the
`RUNNING + active=true` row in the state×flag table.

So a jog during pause is:
```
STATE_PAUSED
  → host sends JOG_MAGIC packets
  → Pico: STATE_RUNNING, runningReason=JOG, active=true
  → queue drains
  → Pico: STATE_PAUSED  (active=true told it to come back here)
```

### Resume is a separate explicit step from the return jog

The auto-return to `pausePos` is **not** the resume. It is a host-computed jog
that happens to land at `pausePos`. Only after it completes does the host send
the explicit `resume` command, which transitions the Pico into job-mode
`STATE_RUNNING` and continues the stream.

```
… position valid, at pausePos …
  → host sends `resume`
  → Pico: STATE_RUNNING (job mode), continues stream from the pause boundary
```

### Path A — powered jog (coils stay energised)

```
Pico → STATE_PAUSED, pausePos saved
  → host streams jog packets → operator drives head to tool-change position
       (axes_homed stays valid — powered, position tracked throughout)
  → operator swaps the tool
  → host re-pings the NEW tool's required peripheral nodes
       (resolve profile.required_peripheral_roles → machine.peripherals)
       · present & ready → continue
       · absent          → block resume, tell operator
  → host computes the return delta and streams jog packets back to pausePos
  → host sends `resume` → job continues
```

### Path B — de-energise + move by hand

The `disable` (de-energise) command is sent **through the host**, so the host
knows position is now invalid the moment it sends it — no need to query. Two
gates follow, in order:

1. **Re-energise gate.** After `disable`, the machine must be re-energised
   (`enable`) before *any* motion. The host blocks **both jog and resume** until
   `enable` is sent. (Jogging a de-energised machine is meaningless — no torque.)
2. **Position-validity gate.** `disable` cleared `axes_homed`; resume stays
   blocked until position is re-established (operator jogs to a known reference
   and calls `setorigin`, or a future auto-home cycle).

```
operator sends `disable` via host
  → host: position invalid (it sent the command); block jog AND resume
  → operator swaps tool, moves gantry by hand
  → operator sends `enable`
  → host: jog unblocked (resume still blocked — axes_homed cleared)
  → host streams jog packets → operator drives to reference
  → operator `setorigin` → axes_homed restored
  → host re-pings the NEW tool's peripheral nodes
  → host streams jog packets to return to pausePos
  → host sends `resume` → job continues
```

---

## 5. Config Touch Points

These are the config-side hooks the host orchestration relies on. Some exist,
some are proposed additions to `pipeline/stages/config.py`:

- **`ToolProfile.required_peripheral_roles: tuple`** *(proposed)* — roles (e.g.
  `("oscillator",)`) a tool needs present on the bus. Declared by **role**, not
  node id, so the profile stays machine-agnostic. Empty for tools with no
  controller (PEN). Pre-flight resolves roles against `machine.peripherals`.
- **`MachineConfig.peripherals: tuple[BusNode, ...]`** *(exists, inert)* — the
  non-axis bus nodes (knife controller, suction). Role-matched by pre-flight.
- **`select_head(machine, tool_name) -> int`** *(proposed helper)* — find the
  head whose `profile.name == tool_name`. Used per tool group at plan time;
  does not mutate config.
- **`ToolProfile.required_axes`** *(exists)* — the per-tool axis mask used for
  the pre-flight homed check and the resume check.

---

## 6. Protocol — frozen (step 1 done)

The Phase 1 wire contract is frozen in [wire_protocol.md](wire_protocol.md). The
relevant additions for this orchestration:

- **`getstate`** (text control command) — replies
  `state=<s> homed=<hex> alarm=<a> running=<r>`. The host polls this during
  pre-flight and the PAUSE choreography to know what is blocking a resume.
  (Phase 1 subset of the Phase 2 `CMD_HANDSHAKE` — no config CRC32.)
- **`pingnode <id>`** (text) — relays an RS485 ping to a bus node; the pre-flight
  node-presence check (axis + peripheral nodes).
- **`JOG_MAGIC` = 0xAE** — host-driven jog bursts (manual jog + return-to-pausePos).
- Control plane is text lines; data plane (MSEG/JOG/MCFG) is binary,
  magic-dispatched. See wire_protocol.md "Two planes on one USB pipe".

pause/resume/cancel/stop/unalarm/enable/disable/setorigin are text control
commands with the allowed-state matrix in wire_protocol.md.

---

## 7. Auto-return: Phase 1 (host) vs Phase 2 (Pico)

state_redesign.md was written for the **combined Phase 1 + Phase 2** design, so
its "auto-jog to pausePos → continue" describes the eventual behaviour. The
split by phase:

- **Phase 2** (onboard planner): the Pico generates the return-to-`pausePos`
  motion itself — a true auto-return.
- **Phase 1** (host production): the Pico **cannot generate** motion. The host
  computes the return path, streams it as jog packets (`RUNNING` +
  `runningReason=JOG`, `active` stays true → back to `PAUSED`), and only then
  sends `resume`. The "auto-return" is a host action.

No change to state_redesign.md is needed — it is correct as the combined-phase
spec; this doc records how Phase 1 realises the same end state without an
onboard planner.

---

## 8. Implementation Order

Everything hangs off the wire contract, so that is frozen first. Then the Pico
state machine is built bottom-up (state core → position → stream → jog), then
the host client wraps the commands, then the multi-tool pipeline and the
orchestrator tie it together. Dependency-ordered:

| # | Step | Side | Depends on | Delivers |
|---|---|---|---|---|
| 1 | **Freeze the Phase 1 wire contract** — concrete byte values for `CMD_GET_STATE`, `JOG_MAGIC`, the Phase 1 MCFG preamble (`required_axes`), MSEG flags; update wire_protocol.md | spec | — | the contract both sides build against |
| 2 | **Pico state-machine core** — `MachineState` enum + transition map, reason codes (`alarmReason`/`runningReason`), `PausedJobContext`, Core0/Core1 ownership discipline, ESTOP flush | firmware | 1 | IDLE/RUNNING/PAUSED/ALARM/ESTOP backbone |
| 3 | **Pico position model** — `axes_homed`, `machinePos`, `axisBounds`, `setorigin`, enable/disable effects on homing, soft-limit per-emit guard; `CMD_GET_STATE` + `CMD_GET_POS` read paths | firmware | 2 | position truth + the status query host pre-flight needs |
| 4 | **Pico stream ingest + guards** — MCFG preamble parse (`required_axes` pre-flight gate), MSEG execute (exists), `MSEG_FLAG_PAUSE` drain-to-PAUSED, ACK/NACK, `MSEG_FLAG_ESTOP` | firmware | 2,3 | a job streams, pauses cleanly, NACKs correctly |
| 5 | **Pico jog path** — `JOG_MAGIC` accept in IDLE/PAUSED, `runningReason=JOG`, return-to-PAUSED via `active` flag, `jogSeq` duplicate guard | firmware | 2,3 | host-driven jog during pause |
| 6 | **Host protocol client** — Python lib wrapping ping, get_state, get_pos, enable/disable, setorigin, pause/resume/cancel, stream-with-backpressure, jog | host | 1 | one place that speaks the wire protocol |
| 7 | **Host config additions** — `ToolProfile.required_peripheral_roles`, `select_head()`, populate `machine.peripherals` for the real machine | host | — | multi-tool config hooks |
| 8 | **Host multi-tool pipeline** — SVG layer→tool ingest (Stage 1), per-tool-group planning, `MSEG_FLAG_PAUSE` boundary insertion | host | 7 | a multi-tool `.bin` with pause boundaries |
| 9 | **Host orchestrator** — connect → pre-flight → execute → PAUSE choreography → resume; operator dialog | host | 6,7,8 | the end-to-end Phase 1 run |
| 10 | **Integration** — single-tool job end-to-end on hardware, then multi-tool with a real swap | both | all | validated Phase 1 |

Steps 2–5 (firmware) and 6–8 (host) can largely proceed in parallel once the
wire contract (1) is frozen; they converge at the orchestrator (9). The gaps
flagged below get filled as their step is reached.

---

## 9. Host Application Architecture (step 6 expanded)

**Decision:** full package restructure, GUI-first. The scattered `host/` scripts
become a proper Python package; the protocol library is the one thing that owns
the Pico link; the operator GUI is the priority frontend (the Phase 1
pause/swap/resume workflow is inherently interactive); a thin CLI stays for
bring-up and scripting.

### Why a library, not a CLI or GUI

Today `sender.py`, `serialise.py`, `jog.py`, and `jog_ui.py` each open serial
and build packets independently. The restructure makes **one** component own the
wire contract; CLI and GUI are thin frontends over it.

### Target layout

```
host/
  __init__.py            # marks the package; adds pipeline/stages to sys.path
                         #   (pipeline stays unpackaged — see import strategy)
  protocol/
    __init__.py
    link.py              # serial open/close, two-plane dispatch, RX read loop
    packets.py           # ← serialise.py: binary packers/parsers (MSEG/JOG/MCFG/ACK/NACK)
    commands.py          # text control plane: getstate/pause/resume/... + reply parsing
    stream.py            # ← sender.py: windowed ACK/NACK streaming engine
    state.py             # MachineState/AlarmReason/RunningReason enums + getstate parse
  production/            # offline, no serial — pure pipeline orchestration
    svg_to_packets.py    # ← unchanged logic; imports adjust
    validate_plan.py
    verify_packets.py
  gui.py                 # ← jog_ui.py, grown into the operator frontend
  cli.py                 # thin argparse frontend over protocol + production
  diagnostics/
    sim_duplicates.py
    test_comms.py
```

### File mapping

| Current | New | Notes |
|---|---|---|
| `serialise.py` | `protocol/packets.py` | the binary packers — 6 consumers, do first |
| `sender.py` | `protocol/stream.py` | windowed ACK/NACK engine |
| `jog.py` (`make_jog`) | `protocol/packets.py` (builder) + `cli.py` (CLI) | split lib core from CLI |
| `jog_ui.py` | `gui.py` | grown into operator GUI (pre-flight + pause choreography) |
| `svg_to_packets.py` | `production/svg_to_packets.py` | import adjust only |
| `validate_plan.py` | `production/validate_plan.py` | import adjust only |
| `verify_packets.py` | `production/verify_packets.py` | import adjust only |
| `sim_duplicates.py` | `diagnostics/sim_duplicates.py` | |
| `test_comms.py` | `diagnostics/test_comms.py` | |
| `main.py` | deleted | 82-byte hello stub |

### Import strategy

Host becomes a real package: absolute imports (`from host.protocol.packets
import …`), invoked `python -m host.gui` / `python -m host.cli` /
`python -m host.production.svg_to_packets`. `host/__init__.py` inserts
`pipeline/stages` onto `sys.path` so the production modules' existing flat
pipeline imports (`from config import …`, `from flatten import …`) keep working
**without** packaging the pipeline (out of scope here). So host code mixes
package-absolute imports (host-internal) with flat imports (pipeline) — contained
and pragmatic.

### Migration order (each step verified before the next)

1. Create package skeleton (`__init__.py` files + the path shim). Additive, safe.
2. Move `serialise.py → protocol/packets.py`; update its 6 consumers' imports.
   Verify: production byte-identical output + `test_discretize` still pass.
3. Move `sender.py → protocol/stream.py`; update consumers.
4. Move production scripts into `production/`; adjust imports. Verify byte-identical.
5. Move diagnostics; delete `main.py`.
6. Add `protocol/link.py`, `commands.py`, `state.py` (the genuinely new code the
   frozen contract calls for).
7. Grow `jog_ui.py → gui.py` on the library; build the operator workflow.
8. Add the thin `cli.py`.

Verification gate throughout: the production path (`svg_to_packets`) must stay
**byte-identical** on `test_snake.svg` and `test_discretize` must stay green —
the restructure is mechanical, not behavioural.

### Firmware dependency

The library and GUI build against the **frozen contract**, but live integration
(getstate/pause/jog round-trips) needs the firmware track (steps 2–5). Until
then, `protocol/link.py` gets a loopback/simulator backend so the host side is
testable standalone. GUI-first does not mean hardware-first — it means the
operator frontend leads the host code, validated against a simulated Pico.

---

## 10. Bus-driven, not axis-driven

The guiding direction for the host. The config refactor already made the machine
model **bus-first** — `BusNode` is the primitive and axes/heads/peripherals are
*views* over nodes. The host follows the same lens instead of assuming a fixed
X/Y/Z/A machine.

**Realised in Phase 1 (static / construction — no firmware needed):**
- `MachineConfig.present_axes()` enumerates the fitted axes from the config.
- The GUI is **generated from the config's bus topology**: jog rows + status
  readouts per present axis (units from `AxisConfig.rotary`, feeds from config);
  a Peripherals panel with a row per non-axis `BusNode` in `machine.peripherals`.
  Declare a suction node → it appears; drop an axis (`node.present = False`) → its
  row vanishes. `host/sim_config.py` is the editable bus the simulator presents.
- Peripherals are **presence-only** for now (pingable) — the wire protocol has no
  peripheral actuation command yet.

**Deferred to firmware time (live / dynamic):**
- A live per-node status layer — `NodeStatus` (present/enabled/homed/position +
  TMC driver faults), fetched via a future `getbus` query backed by a heartbeat
  cache, surfaced as a GUI **Bus panel**. Bus health is an *orthogonal* layer
  (parallel to operational state) that *feeds* ALARM. Pre-flight then becomes a
  thin filter over the live bus rather than axis-mask plumbing.
- Opening the current **4-axis-only gating** to arbitrary nodes.

For now: presence via `pingnode` / ping-all, and per-axis `enabled`/`homed`
gating from `getstate`. The point on record: the host is moving toward
*"what nodes does this job need on the bus, and are they healthy"*, with axes as
the first node category — not an axis-first machine with peripherals bolted on.

---

## 11. Multi-tool job model

A job may use several tools. The structure that keeps the planner free and makes
dual-head fall out cleanly:

**The plan is an ordered sequence of tool-tagged operations** — each operation is
a contiguous run of motion using one tool. The *planner* emits this and orders it
however it likes (all of A then B; interleaved A/B/A for optimal travel; or a
forced order the job requires). The plan carries **no `MSEG_FLAG_PAUSE`** — tool
changes are just "the next op uses a different tool."

**The sender resolves tool changes against the connected machine**, at stream
time — the planner never decides pauses, the packet-sending host does:

| Tool change | Single head | Dual head (other tool already mounted) |
|---|---|---|
| how | physical swap | host-side head switch + `x_offset` reposition |
| pause? | yes — `MSEG_FLAG_PAUSE` + swap choreography | no — keep streaming |

So **one plan runs on the single-head or dual-head version of the same machine**
(same calibration, different head config) — the sender compiles it differently.
Step packets are baked at production (machine calibration); only the
pause-vs-head-switch decision is deferred to the sender.

**Two-tier pre-flight:**
1. *Upfront, config-level feasibility* — `can_run_tool(machine, profile)` for
   every tool in the plan's tool set: tool requirements ⊆ machine topology
   (`required_axes` ⊆ `present_axes()`, `required_peripheral_roles` ⊆
   `machine.peripherals` roles). Catches "needs CREASE/A-axis but no A axis" or
   "needs suction but no suction node" before a packet streams — no hardware
   needed. Pure config math, no new fields.
2. *Per-activation, physical pre-flight* — present + enabled + homed (+ peripherals)
   for a tool when it becomes active: its first use in the session, or after a
   swap brings it (back) in. You CANNOT physically pre-flight all tools upfront —
   on a single head only one tool is mounted at a time.

The session tracks a **validated-and-still-mounted** tool set: validated on
physical pre-flight, cleared when physically swapped out. Dual-head head-switches
don't invalidate (both tools stay mounted); a single-head A→B→A re-validates A
after the swap back. Both behaviours fall out of the one rule.

**Not modelled (not needed yet):** per-head tool compatibility (any head mounts
any tool today — a `ToolHead` field if it ever matters); whether the physical
tool exists in the shop (inherently physical — the per-activation check catches
it). `ToolHead.profile` (currently mounted) is the sender's input for
head-switch-vs-swap, not part of feasibility.

**Layers:** Planner (SVG layers + logic → ordered tool-tagged ops; today trivial:
one op per layer in order) → Sender (ops × head config → stream + pause
resolution) → Execution (feasibility gate, then stream with lazy per-activation
pre-flight + swap choreography).

---

## Open Items

- ~~`CMD_GET_STATE` payload definition → wire_protocol.md (step 1)~~ ✓ done —
  frozen as the `getstate` text command.
- ~~`ToolProfile.required_peripheral_roles` + `select_head()` → config.py~~ ✓ done.
- `active_head` runtime-vs-plan separation → note only, revisit later.
- SVG layer → tool mapping → Stage 1 ingest (step 8).
- Live bus overview (`NodeStatus`/`getbus`/driver faults/Bus panel) → firmware
  time (see §10).
- Peripheral actuation commands (e.g. suction on/off) → wire-protocol addition
  when the hardware is real.
