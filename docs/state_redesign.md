# Pico State Redesign

**Branch:** `pipeline-redesign`  
**Date:** 2026-06-25  
**Status:** not yet implemented

## Architecture — Layered State Model

The design arrived at organically maps closely to **Harel Statecharts** (the
basis for UML state machines), but driven by dual-core implementation concerns
rather than pure hierarchical decomposition. Six distinct layers, each solving a
problem the simpler approach couldn't:

| Layer | What | Why |
|---|---|---|
| 1. Operational state | `machineState` enum | Coarse mode guard — what is allowed/blocked |
| 2. Reason codes | `alarmReason`, `runningReason` | Metadata about why we're in this state — no sub-states needed |
| 3. Context objects | `PausedJobContext` | Extended state that persists across transitions |
| 4. Position model | `axes_homed`, `machinePos`, `axisBounds` | Orthogonal physical knowledge — changes on its own schedule |
| 5. Request flags | `pauseRequested`, `cancelRequested` | Inter-core async requests — deferred events |
| 6. Ownership discipline | Core 0 vs Core 1 write rules | Race safety without mutexes |

**Analogy to statechart theory:**
- Reason codes → no statechart equivalent, but ubiquitous in practice (`errno`, `sys.alarm` in Grbl, HTTP status)
- Context objects → *extended state variables* in UML/statechart terminology
- Position model → *orthogonal region* — parallel state independent of operational state
- Request flags → *deferred events* — queued for processing when the machine is ready
- Ownership discipline → not formalized in theory; in multi-core/multi-task systems it is enforced by task/ISR ownership rules or the actor model

**Rule of thumb for adding new behaviour:**
- New failure mode → new `AlarmReason` value (layer 2), not a new state
- New running sub-mode → new `RunningReason` value (layer 2), not a new state  
- New persistent mid-transition data → field on the relevant context object (layer 3)
- New per-axis physical knowledge → field in position model (layer 4)
- New inter-core async request → new request flag + Core 1 drain-and-transition pattern (layers 5+6)

---

## Resolved

### Replace `positionValid` with `axes_homed` bitmask + bounds sentinel

`positionValid` conflated two things: "did we ever set an origin" and "is position
currently trustworthy." Replace with the Marlin/Klipper hybrid:

```cpp
uint8_t  axes_homed;         // bit per axis: bit0=X bit1=Y bit2=Z bit3=A
int32_t  machinePos[4];      // always retained — incrementally accurate regardless of homing
int32_t  axisBounds[4][2];   // [i][0]=min [i][1]=max in steps
                             // impossible sentinel (min > max) when axis not homed
                             // real (0, max_travel_steps) when homed
```

Key functions replacing the `positionValid` bool:

```cpp
bool axisHomed(int i)  { return axes_homed & (1 << i); }
bool inBounds(int i)   { return machinePos[i] >= axisBounds[i][0]
                             && machinePos[i] <= axisBounds[i][1]; }
```

**Why step counts are always retained:** `machinePos` counts exactly what Core 1
emitted — it is always incrementally correct. "Invalid" means datum unknown, not
counts wrong. Relative jog moves remain safe even when unhomed. Only
absolute-position-dependent operations (soft limits, job envelope checks) need
the homing bit set.

**`setorigin` (per-axis or all):**
- Sets the relevant bit(s) in `axes_homed`
- Zeros `machinePos[i]` for those axes
- Sets `axisBounds[i]` to real travel limits `(0, max_travel_steps[i])`

**Estop:**
- Clears all bits in `axes_homed`
- Resets all `axisBounds[i]` to impossible sentinel
- `machinePos` survives untouched (counts are still accurate relatively)

**Soft limits** use `inBounds()` directly — no separate validity flag needed.

---

### Segment emission guard — three layers

The segment queue is the narrow waist. Both producers (host and future local
planner) funnel through it, so guards here apply equally to both.

**1. Pre-flight (once, before job starts)**
Checked before the first segment of a job is accepted. Both producers do this
before starting production:
- `machineState == STATE_IDLE`
- `(axes_homed & required_axes) == required_axes`
- `inBounds(i)` for all required axes (soft limits)

Host: Core 0 checks before accepting the first packet of a stream; NACKs with
reason if it fails.
Local planner: checks before starting computation — no point doing the math if
the machine isn't ready.

**2. Per-enqueue (at the queue)**
Catches state changes mid-job (e.g. estop arriving while a long job streams).
Local production naturally stalls here: the planner fills a batch, waits for
queue space, tries to enqueue the next batch — if the machine went into ALARM
between batches the enqueue check fails and production halts without the planner
needing special estop awareness.

**3. Per-emit (Core 1 — the authoritative backstop)**
Cannot be bypassed regardless of producer. Already has the estop check. Soft
limit `inBounds()` check lands here.

---

### Homing — procedure not a state (with one exception)

Homing is a procedure that updates `axes_homed` bits and `axisBounds`. It is not
a state in the general case. Two modes:

**Manual homing (current):**
Operator jogs to corner → issues `setorigin` → sets `axes_homed` bits, zeros
`machinePos`, sets real bounds. Executes from `STATE_IDLE`. No state transition
needed. Works today with the existing `setorigin` command.

**Automatic homing (future — limit switches):**
`home` command → `STATE_HOMING` → Core 1 drives switch-seek sequence
autonomously → on completion sets `axes_homed` bits + real bounds → transitions
to `STATE_IDLE`.

`STATE_HOMING` is needed here specifically to block both producers during the
autonomous cycle. Both producers check `machineState == STATE_IDLE` at pre-flight
— they see `STATE_HOMING` and stop without needing a separate flag. Manual homing
does not need this because Core 1 is not running autonomously; the jog is just
normal segment emission followed by a `setorigin` command.

---

### Alarm reason variable — ALARM is one state, reason explains why

ALARM is a *mode of operation* (blocked, waiting for acknowledgement). The reason
is *metadata* (what caused it). These are different things — conflating them into
sub-states or a separate `STATE_CONFIG_ERROR` would force every ALARM check to
care about which kind of alarm. Recovery is always the same path regardless:
fix the root cause, then `unalarm` or `setorigin` → `STATE_IDLE`.

Grbl uses this exact pattern (`sys.state` + `sys.alarm`).

```cpp
enum AlarmReason : uint8_t {
    ALARM_NONE         = 0,
    ALARM_ESTOP        = 1,   // stop command or poison pill
    ALARM_CONFIG       = 2,   // invalid config on boot or push
    ALARM_SOFT_LIMIT   = 3,   // position exceeded travel bounds
    ALARM_HOMING_FAIL  = 4,   // switch not found during auto-home (future)
};

volatile uint8_t alarmReason = ALARM_NONE;
```

**Rule:** always write `alarmReason` before setting `machineState = STATE_ALARM`.
Clear it on recovery. `status` prints both. Adding a new failure mode is a new
enum value — no state machine changes.

**Recovery guidance per reason:**
- `ALARM_ESTOP`       → inspect machine → `unalarm` or `setorigin`
- `ALARM_CONFIG`      → push a valid config → `unalarm`
- `ALARM_SOFT_LIMIT`  → position still known → `unalarm` then back off
- `ALARM_HOMING_FAIL` → check switches → retry `home`

---

### Config validity — boot check and push check

Config is validated on two occasions:
1. **Boot** — Pico loads config from flash (or firmware defaults if flash is empty).
   Invalid config → `alarmReason = ALARM_CONFIG` → `STATE_ALARM`. Machine cannot
   move until a valid config is pushed and acknowledged.
2. **CMD_SET_CONFIG** — host pushes new config. Pico validates before storing.
   Invalid → `ALARM_CONFIG`. Valid → store to flash, remain in current state
   (no restart needed).

**What counts as invalid config:**
- `steps_per_unit == 0` on any present axis
- `f_cpu == 0`
- node id out of range (1–4) on any present axis
- Duplicate node ids across present axes

Firmware defaults are a valid config — they pass the same checks. "No flash
config" is not an alarm; running on firmware defaults is fine. The alarm only
fires if the loaded or pushed config fails validation.

---

### Soft limits — three-layer enforcement

**Layer 1 — Host production (best ergonomics)**
Before generating any segments, the host queries `status` for current `machinePos`,
processes the full SVG path for total displacement, and checks the job bbox against
`max_travel` from the pulled config. Violations reported in mm at the specific path
position that crosses the boundary — far more useful than a mid-job alarm.

**Layer 2 — Core 0 pre-flight (cheap safety net)**
Before accepting the first packet of a stream, check `inBounds(i)` for all present
axes. Catches the case where the machine is already out of bounds before the job
begins. Cannot check the full job extent (segments arrive one at a time).

Per-enqueue checking is not useful — `machinePos` at enqueue time is too stale
(up to 512 segments behind actual emission) to be meaningful.

**Layer 3 — Core 1 per-emit (authoritative backstop)**
Before emitting each segment, check whether applying its delta would violate bounds:

```cpp
for (int i = 0; i < 4; i++) {
    int32_t endpoint = machinePos[i] + delta[i];
    if (endpoint < axisBounds[i][0] || endpoint > axisBounds[i][1]) {
        setAlarm(ALARM_SOFT_LIMIT);
        return false;
    }
}
```

The violating segment is rejected entirely — not partially executed. `loop1` sees
`STATE_ALARM`, flushes the queue (`mBufHead = mBufTail`), discarding all remaining
segments. Same flush path as estop.

**Soft limit vs estop distinction:**
- Estop: `axes_homed` cleared, `axisBounds` reset to sentinel — position no longer trusted
- Soft limit: `axes_homed` intact, `axisBounds` intact — position still valid, operator
  unalarms and backs off

`axisBounds` is populated from machine config on flash at boot and on every
`CMD_SET_CONFIG`.

---

### PAUSE — mid-job suspension

PAUSE is a distinct state. It is not IDLE (pre-flight checks do not re-run on
resume) and not ALARM (job is resumable). The only valid exits are resume,
cancel, or estop.

**State transitions:**
```
RUNNING  → PAUSED   operator `pause` command, or MSEG_FLAG_PAUSE in stream
PAUSED   → RUNNING  `resume` — position valid + auto-return to pausePos → continue
PAUSED   → IDLE     `cancel` — job abandoned cleanly, no alarm
PAUSED   → ESTOP    `stop`   — always available, same alarm path as normal
```

**On entering PAUSE:**
- `pausePos[4]` saved from current `machinePos`
- Core 0 stops accepting new segments (NACKs with PAUSED reason)
- Core 1 drains the ring buffer to empty, then enters `STATE_PAUSED`
- All ACKed segments will have been executed by the time PAUSED is final
- Two triggers: operator `pause` command (unexpected, mid-stream) or
  `MSEG_FLAG_PAUSE` in the segment stream (predetermined, e.g. tool change
  marker placed by the host pipeline)

**What is allowed during PAUSE:**
- Status queries
- Jogging — machinePos updates normally. Required for future manual tool
  changes (park to tool station, reposition after swap). This is why jogging
  cannot be forbidden during PAUSE even though it moves the machine.
- Re-homing via `setorigin` — axes_homed bits and axisBounds update normally.
  Operator jogs to a known reference and calls setorigin for affected axes.
- Automatic homing (future) via `home` command — transitions through
  STATE_HOMING, returns to STATE_PAUSED on completion with axes_homed updated.
- `disable` command — de-energises motors. Operator explicitly signals they
  are about to move the gantry by hand. Pico clears all `axes_homed` bits and
  resets `axisBounds` to impossible sentinel immediately on `disable`. Position
  is always invalid after de-energising — no ambiguity.
- `stop` — always available

**What is blocked during PAUSE:**
- New job segments — host NACKed with PAUSED reason, local planner sees
  STATE_PAUSED at its per-enqueue check and stalls
- Config changes — mid-job config change would corrupt remaining segments

**Position validity during PAUSE — the central concern:**

The machine may jog, de-energise, or be moved by hand during PAUSE. Position
validity cannot be guaranteed. Resume therefore cannot be unconditional.

Position validity check on resume:
```
resume command
  → (axes_homed & required_axes) == required_axes?
      NO  → reject resume: operator must home affected axes first
      YES → auto-jog to pausePos → continue stream
```

This collapses two concerns (position valid? / are we at pausePos?) into one:
if position is valid the machine knows where it is and can compute the
auto-return delta. There is no separate "are we at pausePos?" check —
auto-return handles it unconditionally once position is valid.

**Homing during PAUSE:**

*Manual homing:* operator jogs (powered) to reference position, calls
`setorigin` for affected axes. No state transition — executes from STATE_PAUSED
same as it would from STATE_IDLE. axes_homed bits and axisBounds set normally.

*De-energise + hand movement:* operator calls `disable` → axes_homed cleared,
bounds reset to sentinel. Operator pushes gantry by hand to reference. Calls
`enable` then `setorigin`. The `disable` command is the explicit signal that
position is being abandoned — no implicit detection needed.

*Automatic homing (future):* `home` command → STATE_HOMING (blocks both
producers) → switch-seek → axes_homed bits set, bounds set → returns to
STATE_PAUSED (not STATE_IDLE, since a job is still suspended). The distinction
between returning to PAUSED vs IDLE is important: a home cycle during PAUSE
must not lose the paused job context.

**Planner pause handling — predetermined vs unexpected:**

*Predetermined pause* (`MSEG_FLAG_PAUSE` placed by the pipeline):
The planner knew the pause was coming — it embedded the flag in the stream at
a deliberate point (e.g. a tool change marker between path groups). The host
already knows the boundary; the ACK for the flagged segment confirms it was
executed. Both producers and the Pico are in full agreement. The pipeline can
plan accordingly — e.g. lift Z and park A before the flag, leave the machine
in a clean state for the operator.

*Unexpected pause* (operator `pause` command mid-stream):
Neither producer anticipated this. The Pico receives the text command on Core 0
while Core 1 is still emitting. Both producers must stop wherever they happen
to be:

- *Host production:* Core 0 stops accepting packets and begins NACKing. Core 1
  drains whatever is already in the ring buffer then enters STATE_PAUSED. The
  machine stops at whatever segment was last in the buffer — not a
  pipeline-clean point. The cut may be mid-stroke. The operator is responsible
  for the state of the tool/material.

- *Local production:* Pico planner sees STATE_PAUSED at its next per-enqueue
  check and stops filling the queue. Core 1 drains the buffer. Same outcome —
  stops at an arbitrary mid-job point. No external party to coordinate with.

In both unexpected cases, the pipeline did not prepare a clean stop point.
This is acceptable — unexpected pause is an operator override. The job context
(pausePos, seqnum) is still saved and resume is still possible.

**Dual production — pause synchronisation:**

*Host production:*
The ACK protocol defines the pause boundary precisely. On operator `pause`:
Core 0 stops accepting packets (NACKs with PAUSED reason), Core 1 drains the
buffer to empty. By the time STATE_PAUSED is final, every ACKed segment has
been executed. The last ACKed seqnum = last segment executed = pause boundary.
Host receives PAUSED NACKs starting at N+1 and knows to resume from N+1.
In-flight packets (sent but not yet ACKed) are handled by the existing
Go-Back-N retransmission — no special handling needed.

For predetermined pauses (MSEG_FLAG_PAUSE): the host placed the flag
deliberately, so it already knows the boundary. The ACK for that segment
confirms it was executed.

*Local production:*
No synchronisation problem — the Pico planner is the only party. There is no
external stream to agree with. The planner checks `machineState` before each
batch enqueue, sees STATE_PAUSED, and stalls. On resume it continues from its
own internal cursor. No protocol, no seqnum, no handshake needed.

This asymmetry (host production needs protocol agreement; local production
does not) is a direct consequence of the dual-production architecture: host
production is a two-party protocol, local production is single-party.

**`setAlarm(reason)` helper** (captured here for implementation):
Rather than writing `alarmReason = X; machineState = STATE_ALARM` at every
alarm site, a single helper enforces the rule that reason is always set before
state transitions:
```cpp
void setAlarm(AlarmReason reason) {
    alarmReason  = reason;
    machineState = STATE_ALARM;
}
```

**Deferred:**
- Tool change as a mid-job stream event (`MSEG_FLAG_TOOL_CHANGE`) — manual
  tool change workflow (park, swap, re-home affected axes, resume) deferred
  until tool change is a real requirement. PAUSE is the mechanism it will
  build on.
- Z tool-length offset on tool change — after swapping a tool of different
  physical length, pausePos[2] is in the old Z coordinate frame. Requires
  tool-length measurement and offset compensation. Deferred.
- Job recovery / "go back to segment X" — host-side feature; seqnum
  infrastructure already supports seeking. Deferred.

---

### required_axes — ToolProfile property, not all present axes

Which axes must be homed before a job is accepted is determined by the tool,
not the machine. `required_axes` is a derived property on `ToolProfile`:

```python
@property
def required_axes(self) -> int:
    mask = 0b0011          # X and Y always required
    if self.lift_height > 0:
        mask |= 0b0100     # Z — pen/tool lift
    if self.tangential:
        mask |= 0b1000     # A — tangent tracking
    return mask
```

PEN (no lift) → `0b0011`, PEN with lift → `0b0111`, KNIFE/CREASE → `0b1111`.
Derived from fields already on the profile — no redundant stored field.

The mask is embedded in the binary job header so the Pico can check it at
pre-flight without knowing the tool type.

**Pre-flight check (new job):**
```
machineState == STATE_IDLE
(axes_homed & required_axes) == required_axes
inBounds(i) for all required axes
```

**Pre-resume check (continuing paused job):**
```
(axes_homed & required_axes) == required_axes
```

Resume is a subset of pre-flight:
- `machineState == STATE_IDLE` is dropped — machine is in STATE_PAUSED, not IDLE
- `inBounds(i)` is dropped — pausePos was a valid running position so it is
  in bounds; the auto-return jog moves back to it, and the Core 1 per-emit
  backstop covers the return path
- `axes_homed & required_axes` is kept — position validity is the only gate

`required_axes` for the resume check is the paused job's tool mask — not
re-evaluated. Tool change during pause (which could introduce a new mask)
is deferred.

**Single tool per session** — one ToolProfile per job. Multi-tool sessions
(ATC) are deferred; if they land, the session mask is the OR of all tool masks.

---

---

### State enum audit — full enum and transition map

```cpp
enum MachineState : uint8_t {
    STATE_IDLE    = 0,
    STATE_RUNNING = 1,
    STATE_ESTOP   = 2,  // transient — inter-core flush signal
    STATE_ALARM   = 3,
    STATE_PAUSED  = 4,
    STATE_HOMING  = 5,  // future — automatic home cycle only
};
```

**STATE_ESTOP** is kept as a transient state — it is the mechanically necessary
inter-core signal from Core 0 to Core 1 to flush the queue and transition to
ALARM. Not user-visible but removing it would require a separate signalling
mechanism.

**Complete transition map:**
```
boot ──────────────────────────────────────────────────► IDLE
                                                          │
                              setorigin / unalarm ◄───────┤◄──────── ALARM
                                                          │              ▲
                                                   queue  │              │ setAlarm()
                                                non-empty │              │
                                                          ▼              │
                                                       RUNNING ─ stop ─► ESTOP
                                                          │  ╲  poison   │
                                                queue     │   ╲          │ flush +
                                               drained    │   pause      │ clear axes_homed
                                                          ▼     ╲        ▼
                                                        IDLE   PAUSED ─ stop ─► ESTOP
                                                                 │  ╲
                                                        resume   │   cancel
                                                     (pos valid) │       ╲
                                                            ╲    ▼        ► IDLE
                                                             ╲ RUNNING
                                                              ╲ (auto-return
                                                               ╲ then continue)
                                                                ╲
                                                        home (future, from IDLE or PAUSED)
                                                                  ╲
                                                                 HOMING
                                                                 /    ╲
                                                        complete/      ╲ setAlarm()
                                                              /         ╲
                                                   IDLE or PAUSED      ALARM
                                                  (via isPaused flag)
```

---

### Jog — separate packet type, accepted during PAUSED

Jog uses a distinct magic byte (separate from `MSEG_MAGIC`). Core 0 dispatches
on magic byte at ingest — job packets and jog packets are never ambiguous even
when Core 0 is NACKing job packets during `STATE_PAUSED`. This is what makes
jogging during PAUSE work without a separate command path.

**Accepted states:** `STATE_IDLE`, `STATE_PAUSED`. Rejected in all others.

**No seqnum on jog — why not and why that's wrong:**
Interactive jog feels like a fire-and-forget command but the ACK-lost case is a
real hole: Pico executes the move, ACK is lost in transit, host retransmits, Pico
executes again — silent position error. Full Go-Back-N is overkill (window would
be 1; no rewind scenario exists for single-shot jog), but *some* duplicate guard
is needed.

**Resolution — 1-byte rolling seqnum:**
Jog packets carry a 1-byte rolling counter (`jogSeq`). Pico tracks
`expectedJogSeq` and rejects duplicates (ACKs them without executing, same
pattern as job duplicate guard). No window, no rewind, just duplicate rejection.
Host increments `jogSeq` after each ACK. Lost jog → host retransmits with same
seq → Pico executes once.

**Jog cancel:** when a new jog arrives during `STATE_RUNNING + RUNNING_JOG`,
Core 1 performs a controlled decel ramp before starting the new move. Position
stays accurate throughout.

---

### isPaused flag — job context separate from operational state

`machineState` handles operational guarding (what is allowed/blocked).
`isPaused` tracks job context ("there is a suspended job, resumePos and seqnum
are saved") independently. These are genuinely separate concerns — Klipper uses
the same pattern: `PauseResume` module owns `is_paused` + `last_position`
alongside the toolhead's operational state, not inside it.

The flag solves the HOMING return-state problem cleanly: on homing completion,
`machineState = isPaused ? STATE_PAUSED : STATE_IDLE`. No `homingReturnState`
variable needed — the flag already carries the context.

Job context lives together in a struct:

```cpp
struct PausedJobContext {
    bool    active;          // isPaused — there is a suspended job
    int32_t resumePos[4];   // where to auto-return on resume
    uint8_t requiredAxes;   // tool mask at pause time — used for resume check
};
```

Fields are co-located because they are cleared together on cancel/estop.

**Flag lifecycle:**
```
pause command / MSEG_FLAG_PAUSE  →  active = true,  resumePos + requiredAxes saved
resume (job continues)           →  active = false
cancel                           →  active = false
stop / estop                     →  active = false  (job context lost)
homing                           →  active untouched
```

**Valid state + flag combinations:**

| machineState | active | Meaning |
|---|---|---|
| IDLE    | false | Ready for new job |
| RUNNING | false | Executing job |
| RUNNING | true  | Auto-return jog before resume |
| PAUSED  | true  | Job suspended, operator intervention |
| HOMING  | false | Homing from idle |
| HOMING  | true  | Homing mid-pause, job context preserved |
| ALARM   | false | Hard stop, no job |
| ALARM   | true  | Illegal — estop during PAUSED must clear flag |

---

## Premortem — issues to resolve before implementation

### ~~1. Jogging during PAUSE — mechanism undefined~~ ✓ RESOLVED
Separate jog magic byte. Core 0 dispatches on magic at ingest — jog packets
accepted during `STATE_PAUSED` while job packets are NACKed. Single-byte rolling
seqnum (`jogSeq`) on jog packets for duplicate rejection. See "Jog" section.

### ~~2. PAUSE timing — who sets STATE_PAUSED and when~~ ✓ RESOLVED
`pauseRequested` flag. Core 0 sets flag + starts NACKing. Core 1 checks flag
after each segment, drains queue to empty, then transitions to `STATE_PAUSED`
itself. Same ownership discipline as the rest of the design — Core 0 requests,
Core 1 transitions.

### ~~3. `setorigin` during PAUSE invalidates `pausePos`~~ ✓ RESOLVED
Known limitation, operator responsibility. Calling `setorigin` during `STATE_PAUSED`
resets `machinePos[i]` to 0 in a **new coordinate frame**; `resumePos[i]` is now stale
and auto-return will move to the wrong physical location. Only an issue if the new frame
does not match the old frame. No firmware warning —
the firmware doesn't have enough context to detect this reliably. Host app may
choose to warn if it deems it necessary whenever `setorigin` is called and machine position
is not equal to 0. Maybe if position was valid, from the period pause was called to
when `setorigin` is called, we could flag reliably?

### ~~4. ALARM during auto-return — `active` must be cleared~~ ✓ RESOLVED
`setAlarm()` unconditionally clears `PausedJobContext.active`. Every alarm path
means the job is unrecoverably interrupted — no resume from ALARM exists. The
soft-limit-during-auto-return case (`RUNNING + active=true`) is no exception:
the auto-return itself failed, so the context is already corrupt. `setAlarm()`
is the single choke point; clearing `active` there covers all cases.

### ~~5. `axisBounds` when `max_travel = 0` — undefined~~ ✓ RESOLVED
`max_travel = 0` is now an invalid config value. Every present axis must have a
real `max_travel > 0` — added to the config validation checks alongside
`steps_per_unit == 0`. `axisBounds` always holds a real range; `inBounds()` needs
no special case. Absent axes (`present = false`) are exempt — their deltas are
always 0 and they are never bounds-checked.

### ~~6. Soft limits snippet uses old pattern~~ ✓ RESOLVED
Updated to `setAlarm(ALARM_SOFT_LIMIT)` in the per-emit snippet.

### ~~7. NACK reason for PAUSED — not in the enum~~ ✓ RESOLVED
Added `MSEG_NACK_PAUSED = 0x04` to the wire protocol NACK reason bytes.
