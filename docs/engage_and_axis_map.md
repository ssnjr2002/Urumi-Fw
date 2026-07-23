# ENGAGE, axis_map, and Dual-Head Slot Binding

**Branch:** `node-types`
**Date:** 2026-07-23
**Status:** PROPOSED. Design agreed; not yet implemented. This is the single
source of truth for the ENGAGE / axis-map work ("Workstream A").

Continues [node_type_architecture.md](node_type_architecture.md) §7, which
sketched `CMD_ENGAGE` and left it as an open decision. Cross-links:
[wire_protocol.md](wire_protocol.md) (host↔Pico framing),
[../web/src/config/config.ts](../web/src/config/config.ts) (host-side node model),
[../src/rp2350/core1/core1.cpp](../src/rp2350/core1/core1.cpp) (stream packer),
[../src/node/types/stepper/stepper.cpp](../src/node/types/stepper/stepper.cpp)
(stepper RX ISR + hooks).

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

### 5.2 The Pico owns the current map and diffs it

The Pico holds `slotNode[4]` — the node id currently engaged to each slot (or
`SLOT_NONE`). `axis_map <x> <y> <z> <a>` is a *desired* map; applying it is a
diff:

```
for slot i in 0..3:
    if desired[i] == slotNode[i]:      continue          # unchanged, no packet
    if slotNode[i] != NONE:            ENGAGE(slotNode[i], SLOT_NONE)   # drop old
    if desired[i] != NONE:             ENGAGE(desired[i], i)           # bind new
    collect ACKs
commit slotNode = desired  iff every packet ACKed
```

So a head switch (`axis_map` changing only slots 2,3) emits exactly the two
disengage + two engage packets; X,Y are untouched. This diff is bus work, so it
executes on **Core 1** (which owns RS485); Core 0 parses the CLI verb and hands
the four ids to Core 1.

**Core0→Core1 transfer.** Four node ids do not fit the existing single-word
`(cmd<<8)|node` FIFO form. Use a dedicated two-word message (rare path —
connect / head-switch, never hot):

```
word0 = (FIFO_AXIS_MAP << 24) | (desired[0] << 16) | (desired[1] << 8) | desired[2]
word1 =  desired[3]
```

`FIFO_AXIS_MAP` is a new top-byte marker beside `FIFO_STEP_DEBUG` (0xF0). Core 1
pops both words, runs the diff, and pushes back a readiness result (all-ACKed, or
which node timed out) the same way `GET_POS` pushes a second word.

### 5.3 `disengage` (bare) — global safe state

A no-arg `disengage` clears the whole map: address every currently-engaged node
with `ENGAGE(SLOT_NONE)`, ACKed, and set `slotNode[*] = NONE`. Used at job end /
estop recovery / a clean start — **not** for head switches (it would drop X,Y and
force a re-engage). This is distinct from the map diff, which is surgical.

---

## 6. Gating: the machine is not ready until the map is committed

At boot the map is empty, so nothing can stream. We gate on it through the **one
true motion gate on the Pico — `machineState`** (see §8; the `axes_*` bitmasks
are advisory and enforce nothing). Reuse the reserved `ALARM_CONFIG` reason
(`shared.h`):

```
boot                     → STATE_ALARM, ALARM_CONFIG      (all motion ingest refused)
axis_map x y z a         → Core 1 diffs, engages nodes, collects ACKs
   all slots ACKed       → mapReady; if reason==ALARM_CONFIG → STATE_IDLE
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

The parked head's nodes stay **ENABLEd** (holding torque) but disengaged. The
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
| `shared.h` | `FIFO_AXIS_MAP` | new top-byte marker | Core0→Core1 two-word map message |
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

## 12. Open questions

1. **`axis_map` allowed states** — proposed IDLE/PAUSED/ALARM, reject RUNNING
   (§6.2). Confirm.
2. **`BUS_ADDR_MAX`** — what the type-blind verbs (§9) settle the bus-address
   ceiling to; ties into whether the Pico ever bounds ids at all or just relays
   and times out.
3. **Pull-only `axis_map` read-back** (§8) — include now or defer.
4. **Handshake** — the connect-time re-assert + config pull (§8) are deferred;
   they land when the connect handshake itself is built.
