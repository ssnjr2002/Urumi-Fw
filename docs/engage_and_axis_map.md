# ENGAGE, axis_map, and Dual-Head Slot Binding

**Branch:** `node-types`
**Date:** 2026-07-23
**Status:** DECIDED (§12 resolved 2026-07-24); Stage 1 in progress. This is the
single source of truth for the ENGAGE / axis-map work ("Workstream A").

Continues [node_type_architecture.md](node_type_architecture.md) §7, which
sketched `CMD_ENGAGE` and left it as an open decision. Cross-links:
[wire_protocol.md](wire_protocol.md) (host↔Pico framing),
[../web/src/config/config.ts](../web/src/config/config.ts) (host-side node model),
[../src/rp2350/core1/core1.cpp](../src/rp2350/core1/core1.cpp) (stream packer),
[../src/node/types/stepper/stepper.cpp](../src/node/types/stepper/stepper.cpp)
(stepper RX ISR + hooks).

---

## 0. Current state & migration tasklist

Partial: the §9 relay-bound cleanup has started; nothing else is built.

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
| **host ↔ Pico** | `axis_map <x> <y> <z> <a>` | whole map (≤4 ids) | *intent*: "these bus nodes are my X/Y/Z/A" |
| **Pico ↔ node** | `CMD_ENGAGE <slot>` / disengage | one node | *mechanism*: "you occupy stream slot N" (or none) |

The host declares the **full binding**; the Pico owns the **current map** and
**diffs** each new `axis_map` into the minimal set of per-node engage/disengage
packets. This is the "single source of truth on the Pico" the node-type doc §7
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
| **Core 0** (USB) | owns `slotNode[4]`, parses `axis_map`, diffs, gates on the result |
| **Core 1** (bus) | dumb relay — sends one `CMD_ENGAGE` packet, ACKs back |

`slotNode[4]` — the node id currently engaged to each slot (or `SLOT_NONE`) — is
**local to Core 0**; nothing is shared across cores. `axis_map <x> <y> <z> <a>`
is a *desired* map; applying it is a diff Core 0 runs directly:

```
for slot i in 0..3:
    if desired[i] == slotNode[i]:      continue          # unchanged, no packet
    if slotNode[i] != NONE:            ENGAGE(slotNode[i], SLOT_NONE)   # drop old
    if desired[i] != NONE:             ENGAGE(desired[i], i)           # bind new
    collect ACK
commit slotNode = desired  iff every ENGAGE ACKed
```

So a head switch (`axis_map` changing only slots 2,3) emits exactly the two
disengage + two engage packets; X,Y are untouched.

**Each `ENGAGE` is an ordinary single-node command over the existing FIFO** —
Core 0 pushes it, Core 1 relays it and pushes back the ACK, identical to how
`CMD_SERVO_SET` already works. The slot rides the payload byte exactly like
`vac_servo` packs its idx (there is **no** `FIFO_AXIS_MAP` two-word message):

```
push:  ((uint32_t)slot << 16) | (CMD_ENGAGE << 8) | node   # slot 0..3, or 0xFF = disengage
core1: slot = (word >> 16) & 0xFF → send [node][CMD_ENGAGE][1][slot][crc], await ACK, push result
```

A diff is up to 8 sequential blocking round-trips (≤4 disengage + ≤4 engage) —
fine, it is a cold path (connect / head-switch, never hot).

### 5.3 Partial failure is safe by idempotency — no rollback

If an `ENGAGE` mid-diff times out, Core 0 **does not commit**: `slotNode` keeps
its old value, the machine stays `ALARM_CONFIG`, and the offending node is
reported. The engages already sent stay applied on their nodes, but since the
committed map is unchanged, a **retry re-diffs against the old map and re-sends
the same packets** — and re-engaging a node to the slot it already holds is
idempotent. So retry-after-partial-failure needs no rollback logic.

Because `slotNode[]` is Core-0-local, both the no-arg `axis_map` read-back (§8)
and the §9 `node_isAxis()` membership test are plain local reads — no query path,
no cross-core hazard.

---

## 6. Gating: the machine is not ready until the map is committed

At boot the map is empty, so nothing can stream. We gate on it through the **one
true motion gate on the Pico — `machineState`** (see §8; the `axes_*` bitmasks
are advisory and enforce nothing). Reuse the reserved `ALARM_CONFIG` reason
(`shared.h`):

```
boot                     → STATE_ALARM, ALARM_CONFIG      (all motion ingest refused)
axis_map x y z a         → Core 0 diffs, engages nodes (via Core 1 relay), collects ACKs
   all slots ACKed       → commit slotNode; if reason==ALARM_CONFIG → STATE_IDLE
   any ACK failed        → stay ALARM_CONFIG, report the offending node
```

Why a state and not a new boolean: motion ingest already gates on `machineState`
alone (`data_plane.cpp` — non-IDLE/RUNNING → `NACK_BAD_STATE`), so booting into
`ALARM_CONFIG` refuses **every** motion path (job, jog, debug-step) for free,
with no new predicate threaded through each ingest site. It also reuses the exact
`ALARM → IDLE`-on-precondition recovery shape that `setorigin` already uses.

The gate condition is **"all four slots ACK-confirmed engaged"**, not "a string
was parsed" — a miswired or missing axis node cannot let the machine leave the
unconfigured state.

### 6.1 The exit-guard wrinkle

Overloading `ALARM` means the two existing `ALARM`-exit paths must **not** clear a
config-ALARM (only a successful `axis_map` does):

- `unalarm` (`control_plane.cpp`) — add `if (alarmReason == ALARM_CONFIG) return err`.
- `setorigin`'s ALARM→IDLE recovery — same guard.

(Alternative considered: a dedicated `STATE_UNCONFIGURED`. Rejected — it keeps
`ALARM`'s exits pristine but costs a new state in every state switch /
`stateName` / ingest check. Two small guards is the smaller footprint.)

### 6.2 `axis_map` allowed states

Valid in **IDLE / PAUSED / ALARM**: ALARM covers boot, PAUSED covers head
switches (§7), IDLE covers reconfiguration between jobs. **Rejected during
RUNNING** — rebinding slots mid-stream corrupts in-flight motion.

---

## 7. Dual-head walkthrough

Shared X,Y gantry + two heads (A: Z_a,A_a on nodes 5,6 — B: Z_b,A_b on nodes
7,8). The slot map is fixed; the head switch rebinds slots 2,3:

```
cut with head A:   axis_map 1 2 5 6      # X=1 Y=2 Z=node5 A=node6 → slots 0..3
switch to head B (at the PAUSED tool-change boundary):
   axis_map 1 2 7 8
   Pico diff: slots 0,1 unchanged (no packet)
              slot 2: disengage 5, engage 7
              slot 3: disengage 6, engage 8
   all four ACK → resume
```

The parked head's nodes can stay **ENABLED** (holding torque) or disabled, upto the host, but disengaged. The
switch happens only at the PAUSED boundary the protocol already defines
(re-engaging mid-stream is forbidden, §6.2).

---

## 8. The handshake / periodic boundary — what the Pico reports

A split worth stating explicitly, because it decides where `axis_map` lives:

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
already carried by `machineState`/`alarmReason` (`ALARM_CONFIG` ⇔ not committed).
Zero new status bytes.

The host-restart-while-Pico-runs case (fresh host, empty map; Pico still holds
its committed one) is a **handshake** concern, resolved by **re-asserting
`axis_map` on connect** (the Pico diffs; unchanged ⇒ near no-op). This is the same
future connect handshake that would carry the **config pull** if the host ever
needs to read back the stored config blob. Both are deferred with the handshake;
this doc does not implement them.

Optional, independent of the handshake: a **pull-only `axis_map` (no-arg)** that
prints the current committed binding — a bring-up/debug affordance, off the hot
path, pairing naturally with the setter. Recommended, not required.

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
| `shared.h` | `ALARM_CONFIG` | `2` (exists, reserved) | boot / map-not-ready gate |
| `control_plane.cpp` | `slotNode[4]` | Core-0-local | committed node↔slot map; diffed per `axis_map` |
| CLI (`control_plane.cpp`) | `axis_map <x> <y> <z> <a>` | — | setter; IDLE/PAUSED/ALARM |
| CLI | `axis_map` (no-arg) | — | optional read-back (§8) |
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
