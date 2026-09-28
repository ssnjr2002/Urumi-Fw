# ENGAGE, slot_map / axes_map, and Dual-Head Slot Binding

**Branch:** `node-types`
**Date:** 2026-07-23
**Status:** DECIDED (§12 resolved 2026-07-24); Stage 1 in progress. This is the
single source of truth for the ENGAGE / axis-map work ("Workstream A").

Continues [node_type_architecture.md](node_type_architecture.md) §7, which
sketched `CMD_ENGAGE` and left it as an open decision. Cross-links:
[wire_protocol.md](wire_protocol.md) (host↔Pico framing),
[../web/src/machine/index.ts](../web/src/machine/index.ts) (host-side node model),
[../src/rp2350/core1/core1.cpp](../src/rp2350/core1/core1.cpp) (stream packer),
[../src/node/types/stepper/stepper.cpp](../src/node/types/stepper/stepper.cpp)
(stepper RX ISR + hooks).

---

## 0. Current state & migration tasklist

Stages 1–3 are built. `axis_map` has since split into `slot_map` and
`axes_map` (§5.4, docs/plans/state-handling.md branch 1d); the stage notes
below keep the names they were written with.

- [x] §9 relay bounds — `BUS_ADDR_MAX`(8)/`AXIS_NODE_MAX`(4)/`node_isAxis()`;
  `enable`/`disable`/`pingnode`/`nodepos` widened, axis bookkeeping gated
  behind `node_isAxis()` (`control_plane.cpp`). *Uncommitted, intermingled
  with knife WIP.*
- [x] **Stage 1** — node runtime slot + `CMD_ENGAGE` + `ENABLE` decouple (§4).
  Stepper boots disengaged; `CMD_ENGAGE 0x20`; gate is `slot==SLOT_NONE`.
- [x] **Stage 2** — Pico **Core-0** `slotNode[4]` diff (emits granular
  `CMD_ENGAGE` via the existing single-word FIFO — no `FIFO_AXIS_MAP`) +
  `axis_map` set/read verbs + `ALARM_CONFIG` boot gate + exit guards (§5–6).
- [x] **Stage 3** — §9 type-blind `enable`/`disable`/`pingnode`: `node_isAxis`
  → `slotNode[]` membership (`nodeSlot()`); `axes_*` bits slot-indexed, not
  `node-1`; `AXIS_NODE_MAX` retired → `MOTION_SLOTS`; `all` iterates the map.
- [ ] **Stage 4** — dual-head PAUSED switch bring-up (§7).
- [ ] `AXIS_NODE_MAX` is **repurposed**, not retired — it becomes the motion-slot
  count (4), the width of the `axes_*` masks (rename to `MOTION_SLOTS` when §9
  lands). `node_isAxis()` becomes a `slotNode[]` membership test.

**§12 decisions (2026-07-24):** Q1 IDLE/PAUSED/ALARM (reject RUNNING+ESTOP);
Q2 keep `BUS_ADDR_MAX`=8 as a CLI typo-reject, Pico otherwise relays+times out;
Q3 include the no-arg `axis_map` read-back; Q4 handshake stays deferred. The
diff moves to **Core 0** (see §5) — `FIFO_AXIS_MAP` is dropped entirely.

---

## 1. Problem

The stream byte is **four 2-bit slots** (`bit(2n)` = step, `bit(2n+1)` = dir),
and today a stepper node's slot is **hardwired from its bus address**:

```c
// stepper.cpp node_setup()
stepBitMask = 1 << ((NODE_ID - 1) * 2);   // slot = NODE_ID - 1
```

The Pico's packer is the mirror of that — it writes axis deltas positionally,
`{dx,dy,dz,da}` → slots 0..3 (`core1.cpp`), with no node id anywhere in the
stream path. The two only meet if **node 1 = X, 2 = Y, 3 = Z, 4 = A**.

This blocks two things we now need:

- **Stepper nodes on ids other than 1..4.** `stepBitMask` for `NODE_ID ≥ 5` is
  `1 << 8` or higher — it overflows the `uint8_t` mask to **0**, so the node
  never steps. Ids are pinned to `{1,2,3,4}`, full stop.
- **Two tool heads, each with its own Z + A.** That is six stepper axes sharing
  a four-slot byte. Only one head cuts at a time, so at any instant ≤ 4 axes are
  live — but *which* physical nodes occupy slots 2 and 3 must change when the
  head switches.

Both are the same underlying fix: **decouple the stream slot from the bus
address, and make it runtime-assigned.** That is `CMD_ENGAGE`.

---

## 2. Scope

**In:**
- Runtime slot on the stepper node, set by `CMD_ENGAGE` (retires the
  `NODE_ID`-derived mask).
- Decoupling `CMD_ENABLE` (energize) from stream gating (now the engaged slot).
- A host↔Pico `axis_map` command + the Pico-owned map/diff that drives the
  per-node engage/disengage packets.
- A motion gate: the machine is not ready until the map is committed.

**Out (deliberately):**
- **Wider stream frame** (>4 axes stepping *simultaneously*, §7 of the node-type
  doc). We alternate heads, never exceeding 4 live axes, so the 4-slot byte is
  sufficient forever on this machine.
- **A node-type registry on the Pico.** Node-type authority stays in the web
  orchestrator (`config.ts` `BusNode {id, type, present}` + connect-time
  `CMD_GET_TYPE` validation). The Pico learns only its own *axis map* — which
  bus ids occupy its four motion slots — which it already needs for the packer.
- **On-device per-move enforcement of `axes_enabled`/`axes_homed`.** These stay
  per-axis *advisory* telemetry (§6); the host is trusted to not launch a job on
  unhomed/de-energized axes, exactly as today.

---

## 3. Two levels, two vocabularies

The confusion to avoid: "engage" means different things at different links.
Keep them named apart.

| Link | Verb | Granularity | Semantics |
|---|---|---|---|
| **host ↔ Pico** | `axes_map <x> <y> <z> <a>` | whole map (≤4 ids) | *intent*: "these bus nodes are my X/Y/Z/A" |
| **host ↔ Pico** | `slot_map <n0> <n1> <n2> <n3>` | whole map (≤4 ids) | *primitive*: "these nodes listen on slots 0..3", any type |
| **Pico ↔ node** | `CMD_ENGAGE <slot>` / disengage | one node | *mechanism*: "you occupy stream slot N" (or none) |

The host declares the **full binding**; the Pico owns the **current map** and
turns each new map into per-node engage/disengage packets (§5.2). This is the "single source of truth on the Pico" the node-type doc §7
already assigns to the master — made concrete.

Why the wire is unavoidably granular (never a broadcast): each engage must be
**addressed and ACKed**. A stream byte carries no node id, so a silently-dropped
engage that left the wrong motor on a slot would move the wrong axis with no way
to catch it. Four nodes can't ACK a broadcast without colliding.

---

## 4. Node side (stepper type)

### 4.1 Runtime slot replaces the compile-time mask

`slot` becomes state, initialized **disengaged**:

```c
#define SLOT_NONE 0xFF
static uint8_t slot        = SLOT_NONE;   // set by CMD_ENGAGE
static uint8_t stepBitMask = 0;           // derived from slot; 0 while disengaged
static uint8_t dirBitMask  = 0;
```
`node_setup()` no longer seeds the mask from `NODE_ID`. A node boots
**disengaged and ignores the stream** until told otherwise.

**Decided:** the slot is a named enum — self-documenting in the packer/ISR at
zero cost:

```c
enum Slot : uint8_t {
  SLOT_X = 0,
  SLOT_Y,
  SLOT_Z,
  SLOT_A,
  SLOT_NONE = 255
};
```


### 4.2 `CMD_ENGAGE` — a stepper type-specific command

New command in the type-specific range (§3 of the node-type doc, `0x20+`):

```c
#define CMD_ENGAGE 0x20   // payload: [slot]; 0..3 = slot, 0xFF = disengage
```

Handled in `node_handle_command` (falls through the generic table). Payload is
one byte:

```c
case CMD_ENGAGE: {
    uint8_t s = pkt[3];
    slot = s;
    if (s == SLOT_NONE) { stepBitMask = dirBitMask = 0; }
    else { stepBitMask = 1 << (s*2); dirBitMask = 1 << (s*2 + 1); }
    replyAck(CMD_ENGAGE, reply, replyLen);   // [NODE_ID][CMD_ENGAGE][0][crc]
    return true;
}
```

A node with `slot == SLOT_NONE` ignores stream bytes → its `absolutePosition`
freezes, which is exactly right for a parked axis.

### 4.3 ENABLE decouples from the stream gate

Today `node_set_enabled` does double duty: it energizes the driver **and**
`streamEnabled` gates whether stream bytes step. The dual-head parked state needs
these in *opposite* positions — a parked head is **enabled** (Z holds its height
with torque) but **disengaged** (ignores the stream). So they split:

```c
void node_set_enabled(bool on) {          // energize only — no stream role
    if (on) HAL_MOTOR_ENABLE();
    else    HAL_MOTOR_DISABLE();
}
```

`streamEnabled` **retires.** The stream gate moves to the slot, in the RX ISR:

```c
// was: if (!streamEnabled) return;
if (slot == SLOT_NONE) return;            // disengaged → ignore stream, freeze pos
```

This realizes the node-type doc's "ENGAGE ⊥ ENABLE": `ENABLE` = holding torque,
`ENGAGE` = reads the stream, **both** required to actually move.

> **Edge (noted, not guarded):** *engaged-but-disabled* is a nonsense combo —
> steps pulse a de-energized driver (no motion, but `absolutePosition` would
> lie). It never arises in the pause→engage→resume sequence, and a disabled
> driver won't move regardless. We do **not** re-couple the two to prevent it.

---

### 4.4 Make safe and bus silence

`CMD_MAKE_SAFE` (generic, `0x09`) is disable plus disengage in one transaction:
the node de-energises, clears `ENABLED | DATUM`, and calls its `node_release()`.
The stepper drops its slot (as `CMD_ENGAGE 0xFF`); a vacuum closes its servos
and, with `NODE_HAS_PROBE_REPLY`, drops its probe slot. The reply is the status
payload, so it proves the result: released = no slot in the tail, or slot
`0xFF`, and `NODE_FLAG_ENABLED` clear. The probe vacuum's tail carries its slot
as a third byte for this.

A node built with `NODE_HAS_SILENCE_TIMEOUT` (off by default) that hears no
byte for `BUS_SILENCE_MS` (1 s) runs the same routine by itself. Any byte that passes the FERR check feeds the timer, stream or command,
to any address. Core 1 sends a zero stream byte after `BUS_KEEPALIVE_MS`
without sending, between segments only; during a job every step sends a byte.
It does not send during `core1FlashPark`, so a config commit longer than the
timeout leaves every node safe and the machine needs a re-home.

A node's silence release leaves the Pico's binding as it was: the Pico cannot
see it. A `makesafe` or estop release changes the binding only as §5.5 says.

---

## 5. Pico side

### 5.1 The packer is unchanged

Core 1's stream packer stays positional: axis `i` → slot `i`, `{dx,dy,dz,da}` →
slots 0..3. `ENGAGE` only rebinds *which physical node* occupies each slot; the
logical axis→slot map and the MSEG format (`dx/dy/dz/da`) are untouched.

### 5.2 Core 0 owns the current map and diffs it

**The map abstraction is a host↔Pico (USB) concern, so it lives on Core 0** —
the core that owns the USB side. The bus side (Core 1) has no idea the map
exists; it only speaks the granular `CMD_ENGAGE` verb. This is the clean split:

| Core | Role |
|---|---|
| **Core 0** (USB) | owns `slotNode[4]` and the axes request, parses `slot_map` / `axes_map`, applies, gates on the result |
| **Core 1** (bus) | dumb relay — sends one `CMD_ENGAGE` packet, ACKs back |

`slotNode[4]` — the node id currently engaged to each slot (or `SLOT_NONE`) — is
**local to Core 0**; nothing is shared across cores. A slot map is a *desired*
binding; applying it (`ops/slot_map.cpp`) is deliberately **not a diff** — it
always re-sends every engage:

```
for slot i fenced, desired[i] != NONE:  MAKE_SAFE(slotNode[i])     # fence retry (§5.5)
for slot i bound, not fenced:          ENGAGE(slotNode[i], NONE)  # park; unconfirmed fences
for slot i in 0..3 not fenced/skipped: ENGAGE(desired[i], i)     # bind; no answer fences
a failed engage stops the apply: later slots stay unbound
```

**Why not a diff.** A skip-if-unchanged diff was tried and removed: because the
Pico's `slotNode` persists while nodes can independently reset/reflash, a re-issued
identical map diffed to *nothing* and sent no engage — so a node that had
silently dropped to `SLOT_NONE` stayed disengaged while the map claimed it was
bound, and motion streamed into a slot nobody listened to (confirmed on the
bench). Always re-sending every engage makes a map self-correcting: the node
state can never drift from what the map claims. It costs a few extra cold-path
round-trips (connect / head-switch), which do not matter.

**Each `ENGAGE` is an ordinary single-node command over the existing FIFO** —
Core 0 pushes it, Core 1 relays it and pushes back the ACK, identical to how
`CMD_SERVO_SET` already works. The slot rides the payload byte exactly like
`vac_servo` packs its idx (there is **no** `FIFO_AXIS_MAP` two-word message):

```
push:  ((uint32_t)slot << 16) | (CMD_ENGAGE << 8) | node   # slot 0..3, or 0xFF = disengage
core1: slot = (word >> 16) & 0xFF → send [node][CMD_ENGAGE][1][slot][crc], await ACK, push result
```

An apply is up to 8 sequential blocking round-trips (≤4 disengage + ≤4
engage) — fine, it is a cold path (connect / head-switch, never hot).

### 5.3 Partial failure is safe by idempotency — no rollback

If an `ENGAGE` times out, that slot is fenced (§5.5) and the later ones are
left unbound, the request stays unmet and the machine goes `ALARM_NODE_FAULT` (§6). A retry
re-applies the same request and re-sends the same packets, and re-engaging a
node to the slot it already holds is idempotent. So retry-after-partial-failure
needs no rollback logic.

Because `slotNode[]` and the axes request are Core-0-local, the read-backs (§8)
and `node_isAxis()` are plain local reads — no query path, no cross-core hazard.

### 5.4 Two layers: `slot_map` and `axes_map`

```
axes_map   the axis request:   X=1 Y=2 Z=3 A=4   (ops/axes_map.cpp, request in position.cpp)
slot_map   the slot binding:   1 2 3 4           (ops/slot_map.cpp)
axis k is bound  <=>  slot k holds the node axes_map names for k, and it is not pending
```

* **`slot_map <n0> <n1> <n2> <n3>`** binds any node type to the slots, with no
  config. `-`/`0` = empty, `err dup` for a node twice. It stores the **slot
  request** and applies it. No-arg: read back the binding
  (`slot_map - - 3 6`). Refused in RUNNING and PROBING.
* **`axes_map <x> <y> <z> <a>`** is the axis request. Refused without a config
  (`err unconfigured`); every id must be an axis node the config marks present
  (`err node <id> not_in_config`). Every named axis starts **pending** and gets
  a `CMD_NODE_STATUS`:
  * a stepper clears pending;
  * a confirmed non-stepper refuses the map, nothing changes
    (`err node <id> not_stepper`);
  * no answer keeps it pending (`err node <id> timeout`).

  A node holding a fenced slot gets make-safe instead, whose status reply is
  the type check (§5.5); unconfirmed, it stays pending with `err fenced …`.

  The request is then the slot request, applied per slot: a pending axis's
  slot is parked, not engaged, and the other slots bind as requested. The
  request stays unmet, so the machine is `ALARM_NODE_FAULT` until `unalarm`
  re-checks. The config's default map
  keeps a wrong type pending rather than refusing, so a wrong config boots into
  `NODE_FAULT`. No-arg: read back the request, pending as `?n`
  (`axes_map 1 2 ?5 4`).
* **Axis state is for bound axes only.** `machinePos`, `axes_homed`,
  `homingLatched` and `axes_enabled` read 0/clear for an unbound axis; the
  node-frame datum is untouched. `axes_enable`, `setorigin` and the Z lookups
  address bound axes; `step` and `hallscan` address a node in any slot.
* **Ingest refuses** a nonzero delta for an unbound axis (`NACK_BAD_STATE`), so
  a job's `da` cannot reach a vacuum lent slot 3. No `axes_map` since the last
  wipe means no axis is bound.
* A probe is the slot map `- - <z> <vac>`: only Z stays bound. Its exit
  re-applies the axes request, which parks the vacuum like any slot holder
  ([tool_probe.md](tool_probe.md) §5).

### 5.5 Slots are freed only by confirmation: the fence

A slot is left only on its node's word. A park that is not confirmed, or an
engage with no answer (a NAK is a confirmed refusal), **fences** the slot: it
keeps the node's id, and since nothing is known about that node its origin is
invalidated and the slot's views are cleared. On the `ALARM_ESTOP` edge every
bound slot is fenced, since the estop sweep's make-safe replies stay on Core 1.

A fenced slot:

* takes no engage, and satisfies a request of `-` only;
* unbinds its axis (`axisNode` is `-`), so ingest refuses steps for it (§5.4);
* reads back as `!n` in `slot_map` (`slot_map 1 2 !3 4`).

A fence clears only on a confirmed release from its node: a make-safe whose
reply shows no slot, or an engage that shows the node in another slot. A map
that requests a fenced slot, for its own node or another, first sends
make-safe to the fenced node; unconfirmed, the map answers `err fenced <s0>
<s1> <s2> <s3>` naming the fenced node of each such slot (`-` elsewhere) and
the rest of the map still applies. Never on time or silence: a node can keep
missing whole frames while bytes still feed its silence timer.

`makesafe <id>` confirmed unbinds the node's slot and drops the node from the
slot and axes requests (`-`, not pending), so no alarm follows; unconfirmed,
its slot is fenced and `NODE_FAULT` follows. A soft reset forgets the requests
but keeps the slot table, bindings and fences: only power-on clears it.

---

## 6. Gating: a requested map must be complete

A requested map that did not fully engage must not stream. We gate on it through
the **one true motion gate on the Pico — `machineState`** (see §8; the `axes_*`
bitmasks are advisory and enforce nothing). Unmapped is not an alarm: with no map
requested since the last soft reset the machine is IDLE with nothing bound.
Motion gating on an unmapped machine is later work
(docs/plans/state-handling.md). The map comes from the stored config
(docs/plans/pico-config.md): the Pico's controller commits the config's
`defaultHead` map itself, through the same `axesMapApply` path as a host
`axes_map`.

```
boot / soft reset, no valid config → nothing requested → IDLE, unmapped
boot / soft reset, valid config    → STATE_ALARM, ALARM_NODE_FAULT, then the
                                     controller commits the defaultHead map
   map complete                    → resumeOrHold() → IDLE (or LIMIT_LATCHED)
   a node did not ACK              → stay ALARM_NODE_FAULT, map incomplete
accepted CFG_SET                   → re-derive the defaultHead map the same way
axes_map x y z a                   → refused without a config; each id must be
                                     a stepper axis node the config marks present
slot_map n0 n1 n2 n3               → any node, no config
   binding = request               → clears ALARM_NODE_FAULT
   binding ≠ request               → raises ALARM_NODE_FAULT
```

**Complete** means the binding equals the **slot request**, the one last
written by `slot_map` or `axes_map` (`slotMapComplete`); with none requested it
is complete. `NODE_FAULT` never reads axes, so a probe's binding is complete
like any other. The controller requests the config's `[x, y, head.z, head.a]`
for `defaultHead`; a host map replaces the request, and may be partial
(`slot_map 1 - - -` to bench one node): once every node it names engages, the
alarm clears. A node that fails to engage fences its slot and leaves every
later slot unbound, so a stale id can never make a map read as complete. A
fenced slot counts as `-` (§5.5).
The command still answers `ok` when the result is incomplete: committing the
map is what was asked for — the resulting machine being alarmed is a state
fact, carried by the reason code.

Why a state and not a new boolean: motion ingest already gates on `machineState`
alone (`data_plane.cpp` — non-IDLE/RUNNING → `NACK_BAD_STATE`), so an alarm
refuses **every** motion path (job, jog, debug-step) for free, with no new
predicate threaded through each ingest site.

### 6.1 The exit-guard wrinkle

The exit rule checks state, not history. Every path back to IDLE goes through
`resumeOrHold()`, which holds `ALARM_NODE_FAULT` while the map is incomplete,
before it considers a latched limit. On top of that:

- `unalarm` answers `err unconfigured` without a config (the controller-command
  gate). With the request unmet it retries once — the slot request, or for one
  from `axes_map` the pending type checks and then the apply — and answers
  `err unmapped` if it is still unmet.
- `setorigin`'s ALARM→IDLE recovery defers to `resumeOrHold()`.
- Probe teardown re-applies the axes request and lands in `resumeOrHold()` if
  that left the request unmet.

(Alternative considered: a dedicated `STATE_UNCONFIGURED`. Rejected — it keeps
`ALARM`'s exits pristine but costs a new state in every state switch /
`stateName` / ingest check.)

### 6.2 `slot_map` / `axes_map` allowed states

Valid in **IDLE / PAUSED / ALARM**: ALARM covers boot, PAUSED covers head
switches (§7), IDLE covers reconfiguration between jobs. **Rejected during
RUNNING** — rebinding slots mid-stream corrupts in-flight motion.

---

## 7. Dual-head walkthrough

Shared X,Y gantry + two heads (A: Z_a,A_a on nodes 5,6 — B: Z_b,A_b on nodes
7,8). The slot map is fixed; the head switch rebinds slots 2,3:

```
cut with head A:   axes_map 1 2 5 6      # X=1 Y=2 Z=node5 A=node6 → slots 0..3
switch to head B (at the PAUSED tool-change boundary):
   axes_map 1 2 7 8
   Pico: status 1 2 7 8 (steppers), park 1 2 5 6, engage 1 2 7 8
   all four ACK → resume
```

The parked head's nodes can stay **ENABLED** (holding torque) or disabled, upto the host, but disengaged. The
switch happens only at the PAUSED boundary the protocol already defines
(re-engaging mid-stream is forbidden, §6.2).

---

## 8. The handshake / periodic boundary — what the Pico reports

A split worth stating explicitly, because it decides where the map lives:

- **Periodic** (`STATUS_RSP`, polled hot) carries **state the Pico evolves on its
  own that the host cannot derive** — `machineState`, `axes_enabled/homed`
  (an estop clears them autonomously), `alarmReason`, buffer level, position,
  seq, queued-µs.
- **Handshake** (one-time, at connect — **not yet implemented**) is where
  host-authored, Pico-static configuration is (re-)asserted.

**The axis map is host-authored and never autonomously mutated**, so it fails the
periodic criterion and does **not** belong in `STATUS_RSP`. Adding it would be the
host echoing back what it just said, on the hottest frame. The only
status-relevant *projection* of the map — "is the machine ready to move?" — is
already carried by `machineState`/`alarmReason` (`ALARM_NODE_FAULT` ⇔ incomplete).
Zero new status bytes.

The host-restart-while-Pico-runs case (fresh host, empty map; Pico still holds
its committed one) is a **handshake** concern, resolved by **re-asserting
`axes_map` on connect** (the Pico re-applies it). This is the same
future connect handshake that would carry the **config pull** if the host ever
needs to read back the stored config blob. Both are deferred with the handshake;
this doc does not implement them.

Independent of the handshake: the no-arg forms read back without the hot path.
`axes_map` prints the axis request, pending as `?n`; `slot_map` prints the
binding, fenced as `!n`. A host needs only `axes_map`; `slot_map` is for the console.

---

## 9. Generic relay verbs go type-blind (companion cleanup)

The `1..4` node gate and the axis bookkeeping welded onto `enable`/`disable`/
`pingnode` (`control_plane.cpp`) are the last axis-centric assumption in the
control plane. As part of this work they become **type-blind relays**:

- Accept **any valid bus address** (retire the `1..4` literal; the vacuum verbs'
  provisional `BUS_ADDR_MAX` folds into whatever bound this settles on).
- `enable`/`disable` relay `CMD_ENABLE`/`CMD_DISABLE` always; the stepper-only
  `axes_enabled`/`axes_homed` mutation is applied **only when the target id is in
  the axis map**. So `enable 5` on the vacuum node cleanly spins its pump
  (generic effect, delegated on the node) and touches no axis state; `enable 1`
  still updates `axes_enabled` because node 1 is an axis.

This is the host mirror of the node-side "generic command, delegated effect" —
we do **not** rename `enable` → `stepper_enable` (that would re-couple the verb to
a type, the move the node side deliberately avoided).

---

## 10. New constants and commands

| Where | Symbol | Value | Notes |
|---|---|---|---|
| `common.h` | `CMD_ENGAGE` | `0x20` | stepper type-specific; payload `[slot]`, `0xFF` = disengage |
| `stepper.cpp` | `SLOT_NONE` | `0xFF` | disengaged sentinel |
| `common.h` | `CMD_MAKE_SAFE` | `0x09` | generic; no payload; reply = status payload (§4.4) |
| `common.h` | `BUS_SILENCE_MS` / `BUS_KEEPALIVE_MS` | `1000` / `333` | node timeout (opt-in); Pico keepalive (§4.4) |
| CLI | `makesafe <id>` | — | relays `CMD_MAKE_SAFE`; confirmed releases the slot and drops the node from both requests, else fences (§5.5); IDLE/PAUSED/ALARM |
| `shared_state.h` | (was `ALARM_CONFIG`) | `2` (reserved) | retired: no config boots to IDLE |
| `position.cpp` | `slotNode[4]`, axes request | Core-0-local | slot binding; axis request with pending (§5.4) |
| CLI | `slot_map <n0> <n1> <n2> <n3>` | — | any node, no config; IDLE/PAUSED/ALARM; no-arg reads the binding |
| CLI | `axes_map <x> <y> <z> <a>` | — | axis request; IDLE/PAUSED/ALARM; no-arg reads the request |
| CLI | `disengage` | — | global safe-state clear (§5.3) |

---

## 11. Staging

1. **Node side** — runtime slot + `CMD_ENGAGE` handler; `streamEnabled` → slot
   gate; `node_set_enabled` → energize-only. Self-contained; a stepper build
   that still receives no ENGAGE simply stays disengaged (safe).
2. **Pico side** — `slotNode[4]` + diff on Core 1; `FIFO_AXIS_MAP` transfer;
   `axis_map` CLI verb; `ALARM_CONFIG` boot + the two exit guards.
3. **Generic-verb cleanup** (§9) — type-blind `enable`/`disable`/`pingnode`.
4. **Dual-head bring-up** — verify the PAUSED head-switch sequence end to end.

**Migration note.** After step 1 a node boots **disengaged**, so *existing
single-head 4-axis setups no longer stream until the host issues `axis_map`* (even
`axis_map 1 2 3 4`). This is intentional — the `ALARM_CONFIG` gate forces explicit
configuration — but it is a behavior change for anything that assumed
`NODE_ID`-seeded slots. There is no `NODE_ID` fallback by design.

## 12. Open questions — RESOLVED (2026-07-24)

1. **`axis_map` allowed states** → **IDLE / PAUSED / ALARM; reject RUNNING and
   ESTOP** (§6.2). Reuses the existing `stateIs(...)` guard.
2. **`BUS_ADDR_MAX`** → **keep at 8** as a cheap CLI typo-reject; the Pico does
   not otherwise bound engage targets — it addresses, relays, and times out (an
   unreachable id fails the ACK gate). `AXIS_NODE_MAX` is repurposed as the
   motion-slot count, not retired (§0).
3. **Pull-only `axis_map` read-back** → **include now.** Trivial Core-0-local
   read of `slotNode[]`, printed in setter syntax so it round-trips (§8).
4. **Handshake** → **deferred**, unchanged — lands with the connect handshake.

Also decided: the stream slot is a named `enum Slot` (§4.1); the diff runs on
**Core 0**, not Core 1, and `FIFO_AXIS_MAP` is dropped (§5.2).
