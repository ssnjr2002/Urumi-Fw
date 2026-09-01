# RP2350 Refactor & Scaffolding — implementation order

**Status:** plan. Nothing here is implemented.
**Scope:** `src/rp2350/` structure, plus the Pico-side scaffolding for three
designed-but-unbuilt features.
**Cross-links:** [node_session_and_datum.md](node_session_and_datum.md) (sessions,
NAK, fw id), [bus_alarm.md](bus_alarm.md) (node-initiated stop),
[engage_and_axis_map.md](engage_and_axis_map.md) (slots), [wire_protocol.md](wire_protocol.md).

> **How to read this.** Steps are imperative and ordered. Each stage lands its
> subject in **final form** — no shims, no interim protocols, no signatures
> written to be changed later. Stage 0 is independent. Stages 1–5 are the
> refactor, built bottom-up. Stage 6 is what the refactor was for.

---

## 0. Why

`core0/control_plane.cpp` is 1034 lines — about two thirds of core0 — doing four
unrelated jobs: Core-1 FIFO transport, the position/axis-map model, text arg
parsing, and 29 command handlers in one `if (input.startsWith(...))` chain.
`core1/core1.cpp` is 746 lines holding two step emitters, the RS485 frame layer,
and a 236-line FIFO command switch.

The rest of the tree is already split by **external interface**: `data_plane` and
`status` for USB CDC, `core1/bus/RS485Bus` for the wire, `core0.cpp` for byte
routing. The core boundary is the one interface that never got a module — it
exists as a prose comment block in `shared.h` plus **26 hand-packed call sites in
`control_plane.cpp` and 19 in `core1.cpp`**, each re-deriving the same bit layouts
independently on opposite sides of the same channel.

That missing module is the through-line, and it is built first, complete.

---

## 1. Working method

**Write the new file in its final form. Route to it. Delete the old code.**

Never edit a large file into a new shape in place, and never write an
intermediate version you already know you will replace. Stand the final structure
up beside the old one and migrate unit by unit, keeping the old code as
executable reference until its last caller is gone.

For each unit:

1. Write the new file complete, with its header contract.
2. Add a route to it at the top of the old entry point.
3. Fall through to the old code when the new path does not match.
4. Move one unit across. Build. Test that unit.
5. Delete the old copy of that unit.
6. Repeat until the fallback is unreachable, then delete the fallback.

**Set this rule before starting: the fallback must be gone before the branch
merges.** A "temporary" dual path that survives is how you get two dispatch routes
and a bug that reproduces on only one of them.

**The fallback is not intermediate work.** It is a route you delete, not code you
rewrite — nothing written during a migration gets written twice. The same goes for
the state gate in §6: removing it later is deleting a line, not redoing one.

**Consequence of no intermediates:** Stage 1 is large. It lands the header split,
both sides of the RPC, the `queue_t` transport and the async-capable API in one
pass, because splitting them would mean writing the transport twice. That is the
trade — fewer, bigger, final steps instead of more, smaller, provisional ones.

---

## 2. Stage 0 — Standalone, no dependencies

Do these in any order, before or alongside everything else.

### 2.1 Add the `FERR` check to the node RX ISRs

From [bus_alarm.md](bus_alarm.md) §5. **Confirmed live in the working tree.**

1. In `src/node/types/stepper/stepper.cpp:470` and
   `src/node/rs485/isr_generic.cpp:13`, add after the `RXDATAH` read:

   ```c
   if (status & USART_FERR_bm) return;   // corrupted frame — not data
   ```

2. Keep the existing read order (`RXDATAH` before `RXDATAL`) — reading the low
   byte pops the FIFO and invalidates the status.

**Why:** both ISRs currently test only bit 0 (`DATA8`) and never `FERR` (bit 2) or
`BUFOVF` (bit 6), so a frame with a bad stop bit is accepted as valid data today —
line noise or a marginal DE turnaround can inject steps and silently corrupt
position. `bus_alarm.md` §6 also makes this the thing that shrinks the future
bus-alarm blast radius to ~zero, which is why §11 orders it first.

### 2.2 Sweep disengage on Pico soft reset

1. In `core0.cpp`'s soft-reset wipe block, **before** `axisMapReset()`, relay
   `CMD_ENGAGE`/`SLOT_NONE` to every address `1..BUS_ADDR_MAX`.

**Why:** the Pico's soft reset zeroes `slotNode[]`, but nodes are not
power-cycled — the stepper's `slot` lives in RAM and only `CMD_ENGAGE` changes it
(`src/node/types/stepper/stepper.cpp:388`). `axis_map`'s park loop iterates
`slotNode[]` to disengage the old set, so after a reset **it disengages nothing**,
then binds the new set on top of the stale one.

Reproduction: bind node 5 to slot 2 → `reset` → `axis_map 1 2 3 4`. Node 5 stays
engaged to slot 2 and keeps consuming slot 2's step bits, alongside node 3.
`ALARM_CONFIG` does not cover it (it gates streaming until a map commits; the
commit is what leaves the stale binding). The estop sweep does not either —
`CMD_DISABLE` de-energises without disengaging, so the binding survives a
re-enable. This is the connect-time stale-engagement gap named in
[node_session_and_datum.md](node_session_and_datum.md) §3.

Cost is one bus sweep per connect, on a cold path. Superseded later by §8.2.

> Code-path reading, not yet reproduced on hardware. The repro above takes a few
> minutes on the bench — confirm before fixing.

---

## 3. Stage 1 — Build the core boundary, complete

**Creates:** `board.h`, `core0/usb_protocol.h`, `core1/motion_limits.h`,
`ipc/core1_rpc.{h,cpp}`, `ipc/shared_state.h`, `core1/rpc_server.cpp`
**Deletes:** `shared.h`, `FIFO_*` opcodes, `popStatusPayload` and its core-1 mirror

Everything the core boundary touches lands here in final form. The header split
is part of this stage rather than a precursor because its destination —
`ipc/shared_state.h` — only exists once `ipc/` does.

### 3.1 Split `shared.h` to final homes

1. Move the USB wire protocol block (lines 78–199, 234–244 — MSEG/JOG/TILE/TOOL
   magics, ACK/NACK, STATUS, SEQRESET, ABORT, CFG_*) to **`core0/usb_protocol.h`**.
2. Move the motion constants block (lines 304–347 — `V_REST_SPS`, `DECEL_SPS2_*`,
   `decelForAxis`) to **`core1/motion_limits.h`**.
3. Move the pin defines (lines 9–12) to **`board.h`**.
4. Move the cross-core externs, state enums, `MicroSegment` + flags and ring
   config to **`ipc/shared_state.h`**.
5. Delete `shared.h` and fix includes.

No umbrella header. Each block goes to its destination once.

**Why:** `shared.h` is already misnamed. Checked against the include graph:

| Block | Lines | Users |
|---|---|---|
| Pins | 9–12 | **core1 only** |
| Buffer config | 14–16 | both |
| `MicroSegment` + flags | 18–76 | both |
| USB wire protocol | 78–199, 234–244 | **core0 only** |
| FIFO encoding | 201–232 | both — replaced in §3.3 |
| Machine state enums | 246–302 | both |
| Motion constants | 304–347 | **core1 only** |
| Cross-core externs | 348–454 | both |

`STATUS_RSP`'s only appearance in `core1.cpp` is a comment (`:464`), so the USB
block really is core0-exclusive. `DECEL_SPS2_*` has no direct users at all — it is
consumed only by `decelForAxis()`, defined in `shared.h` itself and called from
`core1.cpp:133`. So ~165 lines of USB wire contract are visible to Core 1 for no
reason, and ~45 lines of motion constants are visible to Core 0.

Two bonuses. `usb_protocol.h` is the file mirrored by
[web/src/wire/format/constants.ts](../web/src/wire/format/constants.ts) — as its
own file, "these two must agree" becomes obvious to anyone touching either side.
And `motion_limits.h` is explicitly marked `TEMPORARY`, to be deleted once Core 1
has a config-read path; isolating it turns a buried comment into a file-sized task.

### 3.2 Document the three channels in `ipc/`'s headers

All three cross the core boundary. They belong in one directory because they are
one concern.

| | Channel | Mechanism | Direction |
|---|---|---|---|
| 1 | Command/reply RPC | `queue_t` pair (§3.3) | **Core-0-initiated only** |
| 2 | State + flags | shared volatile globals | bidirectional, async |
| 3 | Motion data | the `MicroSegment` ring | 0→1, highest bandwidth |

**Channel 1** — Core 1 never opens a transaction; it only answers.

| Class | Direction | Traffic |
|---|---|---|
| RPC, reply expected | 0→1 request, 1→0 reply | ping/enable/disable/servo/ssr/knife/laser, engage, `CMD_NODE_STATUS`, `CMD_DATUM_SET`, `CMD_SWITCH_GET`, home |
| Fire-and-forget | 0→1 only | debug step |

**Channel 2** — Core 1 cannot initiate on channel 1, so everything it must report
(estop, soft-limit trip, position advance) goes out as a level signal Core 0
polls. That is why `reconcileValidity()` exists and runs every loop pass.

> RP2350 has `multicore_doorbell_*` (IRQ on the other core) which would let Core 1
> signal asynchronously. Not needed — level-triggered polling is more robust for a
> fault signal — but know it exists before building *more* polling.

### 3.3 Write the transport on `queue_t`, async-capable, first time

Do **not** port the FIFO word protocol into `ipc/` and swap it later.

1. Use two `queue_t`s, following `pico-examples/multicore/multicore_runner_queue`.
2. Use a **tagged struct** element, not the example's function pointer — our work
   is a fixed set of bus opcodes, and a struct is inspectable and cannot be a
   garbage address.
3. Give the request a `token` field (unused until §8.2) and a request id.
4. Use `queue_try_add` / `queue_try_remove` with a deadline. **Shape the API so a
   caller can post and collect later**, even though every caller blocks today.
5. Delete `FIFO_HOME`, `FIFO_STEP_DEBUG` and the `FIFO_*` namespace shadowing
   `CMD_*` — they exist only because 32 bits was too small for their arguments.
6. Delete `popStatusPayload` and its core-1 mirror (~40 lines of pack/unpack).
7. Assert on the `cmd`/`node` echo instead of discarding it.

**Keep the IDLE/PAUSED/ALARM gate on relay commands for now.** It is removed in
§6, which is a deletion enabled by this stage — not a rewrite of it.

**Why `queue_t`:** the SDK is explicit in `pico/multicore.h` — the inter-core
FIFOs are "a very precious resource," frequently needed by SDK functionality and
RTOSes, and "the majority of cases for transferring data between cores can be
equally well handled by using a queue." Three concrete problems today:

- **RP2350's FIFO is 4 entries deep** (RP2040's is 8). The `FIFO_HOME` sequence
  pushes exactly 4 words back-to-back — precisely at capacity.
- **arduino-pico uses the same hardware FIFO.** `RP2040Support.h`'s `_MFIFO`
  pushes a `_GOTOSLEEP` sentinel for `rp2040.idleOtherCore()`. Latent, not live —
  this project avoids it by hand-rolling the `flash_op_requested` handshake — but
  it is a standing constraint.
- **`pop_blocking` has no timeout.** A wedged Core 1 hangs Core 0 forever.

### 3.4 Define the reply and status types in final form

Both carry fields nothing produces yet. That is deliberate: writing them now is
the final shape, and it makes §8 additive instead of a 30-site rewrite.

1. **Result is an enum, not a bool.**

   ```c
   typedef enum { RPC_OK, RPC_TIMEOUT, RPC_NAK } RpcResult;
   typedef struct {
       RpcResult result;
       uint8_t   nakReason;
       uint8_t   len;
       uint8_t   payload[32];
   } RpcReply;
   ```

   Write every call site against three cases. **Why:** the entire result of a bus
   transaction is currently **one bit** (Core 1 computes `rxLen != 0xFF ? 1u : 0u`
   at eleven sites; Core 0 reads `resp & 0xFFFF`). There is nowhere for a reason to
   live, which is why `control_plane.cpp:405` prints `nak_or_timeout` with a
   comment saying the two are indistinguishable.

2. **Decoded status struct — no `NS_*` offsets anywhere.**

   ```c
   typedef struct {
       uint8_t  type, flags;
       uint16_t session;              // 0 until §8.2
       uint8_t  resetCause, fwId;     // session-ack only, see §8.2
       int32_t  pos;   uint8_t slot;  // stepper tail
   } NodeStatus;
   bool nodeStatusDecode(const uint8_t* buf, uint8_t len, NodeStatus* out);
   ```

   Consumers read `st.pos`, never `buf[NS_STEP_POS]`. **Keep the `len`
   parameter** — it lets the decoder branch on payload length and zero-fill absent
   fields, which is the difference between a staged firmware rollout and a
   flash-everything-at-once.

### 3.5 Write the Core-1 side as `rpc_server.cpp`

Migrate `processBus` (`core1.cpp:451–687`) out as it is rewritten for the new
protocol — it is rewritten either way, so move it in the same pass rather than
rewriting in place and relocating in Stage 2.

**Why the name:** `processBus` is core1's `control_plane` — a 236-line switch with
the same disease — and it is literally the server side of the RPC.
`ipc/core1_rpc.h` declares the contract, core0 calls it, core1 implements it.

---

## 4. Stage 2 — Decompose the rest of `core1.cpp`

**Risk: low** — moves, not rewrites. `rpc_server.cpp` already left in §3.5.

| Lines | Concern | Goes to |
|---|---|---|
| 26–33 | flash-quiesce park | stays |
| 34–72 | `sendPacket` / `receivePacket` | `bus/packet.cpp` |
| 73–329 | `emitMicroSegment`, `processMicroSegments` | `emit/microsegment.cpp` |
| 330–378 | `emitDebugSteps` | `emit/debug_step.cpp` |
| 408–450 | `sendBroadcast`, `busDisableAll` | `bus/packet.cpp` |
| 688–746 | `setup1` / `loop1` | stays |

1. Write `bus/packet.{h,cpp}` — the **frame layer**: addressing, CRC, response
   timeout, broadcast rules. Migrate `sendPacket`, `receivePacket`,
   `sendBroadcast`, `busDisableAll`.
2. Write `emit/microsegment.cpp` and `emit/debug_step.cpp`. Move each emitter with
   its private helpers.

**Why `bus/packet`:** `RS485Bus` is the 9-bit *byte* transport (PIO);
`sendPacket`/`receivePacket` are the *frame* layer. Different levels, and the
frame layer has no module — the same gap the core boundary had.

**Why `emit/` as a directory:** [bus_alarm.md](bus_alarm.md) §4 adds a third
participant (the probe supervisor), and both existing emitters will share the
loopback ring (§8.3).

> **Caveat specific to this split.** `__time_critical_func` and
> `__not_in_flash_func` attributes must travel with their functions across the TU
> boundary, and any `static inline` helper the hot loop calls (`rampRequested`,
> `rampStepInBounds`, `decelForAxis`) must stay inline or RAM-resident. Putting one
> in a .cpp instead of a header is the trap — invisible until someone measures
> jitter. Comment it in `emit/microsegment.cpp`.

---

## 5. Stage 3 — Extract the position model

**Creates:** `core0/position.{h,cpp}` (~200 lines). **Risk: low.**

**Do this before the command split**, so `cmd/axis.cpp` is written once against
the final `position.h` rather than against statics that later move.

1. Move as **one unit**: `slotNode[]`, `nodeSlot`, `axisMapReset`, `nodeOrigin[]`,
   `nodeHomed`, `parkPos[]`, `parkSeen`, `originInvalidate`, `originInvalidateAll`,
   `slotAdoptStatus`, `reconcileValidity`.
2. Consume `NodeStatus` from §3.4 — `position` never sees a byte offset.
3. Export the slot map to `cmd/axis.cpp` and nothing else.

**Why one unit:** the model spans three ownership domains and the validity rule is
a **conjunction across all of them**.

| Domain | State |
|---|---|
| Core-0 statics | `slotNode[]`, `nodeOrigin[]`, `nodeHomed`, `parkPos[]`, `parkSeen` |
| Cross-core shared | `machinePos[]` (Core 1 writes it — `core1.cpp:259`, `:376`), `axes_homed`, `axes_enabled` |
| Node-reported | `NODE_FLAG_DATUM`, the node's own counter |

`slotAdoptStatus` is the join of all three. There is no clean cut between "axis
map" and "datum model" — they are the same fact in two frames, which is what the
`nodeOrigin` design is *for*. Fold the slot map in rather than giving it its own
file: it is ~30 lines and `position` touches `nodeSlot()` on nearly every line.

**Settled — keep the map Core-0-private.** [bus_alarm.md](bus_alarm.md) §4.4 wants
Core 1 to supervise a probe drive by validating byte shape against the bound Z
slot. Do **not** promote the map to cross-core for this. Hand Core 1 an expected
byte mask as a parameter at probe-arm time.

---

## 6. Stage 4 — Strangle the command layer into `cmd/`

**Creates:** `core0/cmd/` (6 files). **~−230 lines.**

Everything this stage depends on is final: the RPC API (§3.3), `NodeStatus`
(§3.4), `position.h` (§5). Each handler is written once.

### 6.1 Stand up the table beside the chain

1. Define `struct Cmd { const char* name; bool (*fn)(const char* args); };`
   No `gate` field — state checks stay in the handlers (§7).
2. Match on `strlen(name)` **plus a "next char is space or NUL" check**.
3. Leave every `stateIs(...)` check in its handler, untouched.
4. Put the table lookup at the **top** of `handleCommand`; fall through to the
   existing if-chain on no match.
5. Migrate commands one at a time, deleting each from the chain as it lands.
6. Delete the chain when it is empty.

**Migration order does not matter.** The table's delimiter check is stricter than
`startsWith`, so a table entry for `enable` will not match `axes_enable 1` — it
falls through to the chain, which still has its own ordering intact. Table-first
means a command present in both places is served by the table, so duplication
during migration is safe, not racy.

**Migrate the five peripheral commands as one unit**, collapsing them into one
generic handler plus table rows:

| Command | Line | Opcode | Arg |
|---|---|---|---|
| `vac_servo <node> <idx> <on\|off>` | 629 | `CMD_SERVO_SET` | `(idx<<4)\|on` |
| `vac_pump <node> <on\|off>` | 657 | `CMD_SSR_SET` | `on` |
| `knife_osc <node> <on\|off>` | 685 | `CMD_KNIFE_OSC` | `on` |
| `laser <node> <on\|off>` | 713 | `CMD_LASER` | `on` |
| `knife_blower <node> <0..100>` | 736 | `CMD_KNIFE_BLOWER` | `duty` |

They are identical modulo opcode and arg encoding — same gate *text*, the same 6-line
comment pasted verbatim four times, same parse, same relay, same `printf`. Only
`vac_servo` (extra `idx`) and `knife_blower` (range, not on/off) vary.

**Why the table:** prefix dispatch is ordering-dependent — `axes_enable` works only
because it is tested at line 528, *before* `enable` at 593 (same for
`bus_enable`), so any new command sharing a prefix with an earlier one is silently
swallowed. And arg offsets are hand-maintained magic numbers: `argAfter(input, 11)`
must equal `strlen("axes_enable")`, twenty-odd times, each a silent bug on rename.

### 6.2 Split the handlers by behaviour, not by name

| File | Commands | |
|---|---|---|
| `cmd/query.cpp` | `ping` `getstate` `getpos` `status`/`?` `status cfg` `pingnode` `nodepos` `nodestat` `vac_switch` | 9 |
| `cmd/lifecycle.cpp` | `stop` `reset`/`rst` `seqreset` `pause` `resume` `cancel` `unalarm` | 7 |
| `cmd/periph.cpp` | `vac_servo` `vac_pump` `knife_osc` `knife_blower` `laser` | 5 |
| `cmd/axis.cpp` | `axis_map` `setorigin` `step` `home` `axes_enable` `bus_enable` `enable` `disable` | 8 |

Add `cmd/table.h` (Cmd struct + handler decls) and `cmd/parse.{h,cpp}`
(`argAfter`, `parseState`, `axisMask`, checked node-id).

Do not repeat the directory name in filenames — `bus/RS485Bus.cpp` sets that
precedent. It is `cmd/query.cpp`, not `cmd/cmd_query.cpp`.

Put `vac_switch` in `query.cpp` despite the `vac_` prefix — it reads a switch
level, it does not actuate.

**Why this grouping:**

| File | needs `core1_rpc` | needs `position` | needs `parse` |
|---|---|---|---|
| `query.cpp` | yes | read-only | yes |
| `lifecycle.cpp` | **no** | **no** | no |
| `periph.cpp` | yes | **no** | yes |
| `axis.cpp` | yes | **yes, writes** | yes |

Only one file mutates the position model. Today origin invalidation is scattered
across `disable`, `axes_enable`, `bus_enable`, `axis_map`, `setorigin` and
`reconcileValidity` in one 1034-line file — and the recurring bug the source
comments describe is exactly "updated one frame, forgot the other."
`lifecycle.cpp` depending on nothing but `ipc/shared_state.h` is the other useful
signal: the estop and pause paths get no transport or position coupling to audit.

### 6.3 Preserve the error strings exactly

Do **not** normalise `err usage` vs `err bad_node` here, despite the ~10
inconsistent sites. Those strings are on the wire and mirrored in
`host/protocol/link.py:343`. Normalise later, as its own change, with a doc update
and the host in step.

---

## 7. Stage 5 — Fix what the transport left stale

**Risk: low.** No gate moves, no gate widens, no wire change.

An earlier draft of this section removed the gate on `pingnode`, `nodepos` and
relayed `enable`/`disable`, on the grounds that their gate was a transport
artifact that §3.3 had dissolved. **That was wrong, and the reasoning is worth
keeping so nobody re-derives it.**

The gate had two halves. Core 0's half — blocking in `pop_blocking`, which put
`stop` behind a relay and was measured at 4 s of queued motion — was genuinely a
transport artifact, and Stage 1 removed it. Core 1's half was never a transport
artifact at all: `rpcServerPoll()` runs a real RS485 exchange, up to
`RESPONSE_TIMEOUT_MS`, on the core that owns the step budget. A `pingnode` costs
Core 1 exactly what a `vac_pump` costs it. "Reads with no physical effect"
confused the node's response with the transaction's cost.

So there is no split. All nine commands share one reason, and it is the physical
one. **Every gate stays exactly where it is.**

### 7.1 Fix the four stale gate comments

The peripheral handlers' gate comment cites "the relay blocks Core 0 on a Core 1
round trip (push + pop)" — a mechanism that no longer exists. §6.1 already
collapsed the four verbatim copies into one; correct that copy, and the matching
text on `nodepos`, to the reason that is actually load-bearing:

> Core 1 services channel 1 only between segments, and the exchange takes up to
> `RESPONSE_TIMEOUT_MS` — many step intervals. Issued while a job is streaming,
> that is a dwell at a segment boundary, which leaves a mark in the material.

This is comment-only. No behaviour changes.

### 7.2 The gate does not cover the window it claims — knowingly

`stateIs(IDLE, PAUSED, ALARM)` keys on `machineState`, and `machineState` has
already left `RUNNING` by the moment the damage becomes possible.

`processMicroSegments()` drains the whole ring before `processBus()` reaches step
3, so a request is never serviced mid-segment. The exposure is the window where
the ring transiently empties **mid-job** because the host underfed it. On that
path the emitter's tail runs (`emit/microsegment.cpp`):

```c
if (machineState == STATE_RUNNING) machineState = returnState;   // → IDLE
```

An underfed job therefore sits in `IDLE` between refills, and the gate admits
every one of the nine — peripherals included. The gate is not wrong so much as
keyed on the wrong fact: `machineState` answers "is the emitter busy right now",
where the question is "is a job in flight". Nothing represents the latter today
(`jobActive` means *suspended*, not *streaming*).

**Decision: leave it.** The firmware stays ignorant of underfed jobs for now. The
predicate is §11's, not this stage's — inventing one here would be a motion-side
change smuggled into a refactor, and it needs a measurement (does the ring
actually underrun at working feed rates?) that nobody has taken. What this stage
owes is that the hole is written down rather than papered over by a comment
claiming coverage the code does not have.

### 7.3 One relay in flight at a time

This is the decision that lets everything else stay as it is.

After §3.3 a relay spans several loop passes rather than one blocking call. That
widens the window between a handler's `stateIs` check and its completion — which
is what made an earlier draft reach for an epoch counter.

It is unnecessary if only one relay is outstanding. The RS485 bus is serial, so
serialising relays costs no throughput, and it buys:

- **`alarmAtEntry` stays sufficient.** The existing pattern (`cmd/axis.cpp`,
  `cmdSetOrigin`) is only defeated by `ALARM → IDLE → ALARM` within one command's
  window. Clearing ALARM takes an operator command, and Core 0 cannot process one
  while a relay is in flight. So the sequence is unreachable.
- **Reply ordering is trivially correct** — one outstanding command, one reply.
  This closes the async-ordering question rather than answering it.

**`stop` is not affected.** It is a direct write to `machineState`
(`cmd/lifecycle.cpp`), not a relay, so it never queues behind one. That was the
only real objection.

### 7.4 Asynchronous faults use the pattern that already exists

A node fault raised by Core 1 mid-command needs nothing new.

**It is an `AlarmReason`, not a state:** add `ALARM_NODE_FAULT = 5`.
`ipc/shared_state.h` already records why — reason codes exist "so no sub-states
are needed" — and no gate reads a reason, so the machine lands in `ALARM` and
every command's existing `ALARM` gate applies unchanged.

**Raise it exactly like soft-limit** (`emit/microsegment.cpp`), which is already
correct:

```c
alarmReason  = ALARM_NODE_FAULT;   // reason first
__dmb();                           // ordering barrier
machineState = STATE_ALARM;        // then the state
```

Core 1 signals only; it must not write validity masks it does not own. Core 0's
`reconcileValidity()` drops the affected origins and `axes_homed` bits on its
next pass.

**One real bug to fix while here.** The writer orders reason-before-state, but
`reconcileValidity()` (`core0/position.cpp`) loads `machineState` then
`alarmReason` with no barrier between — the reverse order, so it can observe the
new state with the stale reason. Add the matching `__dmb()` between the two
loads.

### 7.5 What this stage actually buys

Less than the draft claimed, which is the point of having rewritten it.

- The `machineState` / `alarmReason` read barrier is fixed. This is a real race,
  and the only behaviour change in the stage.
- `ALARM_NODE_FAULT` exists, so Stage 6 has somewhere to raise a node fault.
- One-relay-in-flight is written down as a constraint, which is what keeps
  `alarmAtEntry` sufficient and reply ordering trivial.
- The gate comments say what is actually true, and the gap between what the gate
  covers and what it claims to cover is recorded rather than asserted away.

Already delivered by Stage 1: a wedged Core 1 can no longer hang Core 0, and
`stop` is never queued behind a relay.

No wire change. No `web/src` coordination. No new state values.

---

## 8. Stage 6 — What the scaffolding was for

None of this is implemented node-side. Listed in dependency order.

### 8.1 `CMD_NAK [reason]` — DONE

`CMD_NAK 0x07`, reply-only: `[ID][CMD_NAK][2][orig_cmd][NAK_*][crc]`. Reasons
`UNSUPPORTED` / `BAD_TOKEN` / `BAD_ARG`; only the first is produced today, and
`BAD_TOKEN` is §8.2's to raise.

**Why first:** [node_session_and_datum.md](node_session_and_datum.md) §7 names it
as the prerequisite. An unhandled command used to be dropped exactly like a
bad-CRC frame, so the master timed out and "refused" was indistinguishable from
"absent" — which is why the home path could only print `nak_or_timeout`.

Five places, in the order a frame travels:

1. **`include/common.h`** — the opcode and the reason codes, shared by both ends.
2. **`src/node/dispatch.cpp`** — `dispatchCommand` answers an unhandled command
   instead of dropping it. `routeCommand` still returns 0, so `debug_console.cpp`
   is unchanged: the NAK belongs to the wire, where the ambiguity lives.
   Excludes broadcasts (nothing may answer one) and `CMD_NAK` itself.
3. **`core1/bus/packet.cpp`** — `receivePacket` accepts `CMD_NAK` whatever
   `expectedCmd` is, and reports what actually arrived through a new `outCmd`.
   Without this the frame filter drops the refusal and the caller times out
   anyway — the filter, not the decoder, was the load-bearing part.
4. **`core1/rpc_server.cpp`** — a NAK frame becomes `RPC_NAK` + `nakReason`. The
   reply still echoes the REQUEST's opcode, so `rpcCall`'s echo assert holds.
5. **`ipc/core1_rpc.cpp`** — `rpcResultText()`, one mapping from `RpcResult` to
   the printed word, replacing the literal `"timeout"` at eight call sites. An
   unrecognised reason prints `nak <n>`, never `timeout`.

**Wire-visible:** `web/src/wire/link/commands.ts` `_nodeOk` matches
`reply.endsWith("ok")`, so the happy path survives and a NAK reads as failure —
which it is. `nodepos` gained a distinct `bad_reply` for "answered, but with no
stepper tail", previously folded into `timeout`. The sim backend models neither
unsupported commands nor absent-node NAKs, so it is unchanged.

**Bench check (mixed-firmware bus).** Flash one node with this firmware and
leave another on the old code, then send a command neither implements —
`laser <node> on` at a non-laser stepper is the cleanest, since both nodes are
otherwise identical. New node answers `node N nak unsupported` immediately; old
node still spends `RESPONSE_TIMEOUT_MS` and answers `node N timeout`. The two
firmwares are distinguishable on the wire by exactly that, and neither confuses
the other — which is what makes a staged rollout possible.

### 8.2 Session token

1. Extend the periodic status head by the token only:

   ```
   periodic status head:  [type][flags][session_hi][session_lo]    2 → 4 bytes
   SET_SESSION ack:       …same… + [reset_cause][fw_id]
   ```

2. Issue a 16-bit nonce in `core0.cpp`'s soft-reset wipe block.
3. Add the token clause to `slotAdoptStatus` (~4 lines).
4. Make `SET_SESSION` claim-and-safe: disengage, de-energise, laser off.

**Why this shape:** `fw id` and `reset cause` are only consumed *after* a reboot is
detected, and the token is what detects it — so put them in the `SET_SESSION` ack,
not every status reply. This changes `node_session_and_datum.md` §4, which put all
three in the head. Moving two out **pays for a 16-bit token**, settling §5's open
question about 1-byte collisions (~1/256 on re-issue).

The token is a **third witness** alongside Core 0's `nodeHomed` and the node's
`NODE_FLAG_DATUM`. `slotAdoptStatus` is already the conjunction point — which is
why §5 keeps the model cohesive.

**The header-token half is expensive and the refactor barely helps.** §7 wants
owned commands to carry the token in the header (`[ID][CMD][TOKEN][LEN]…`), which
re-indexes every payload across `dispatch.cpp`, `rs485/frame.h`, every node type,
and core1's send/receive. What it does help: `core1_rpc` fills the field once,
centrally.

**Bootstrapping wrinkle.** A node too old to know `CMD_SET_SESSION` silently drops
it — the exact ambiguity NAK removes, but an old node is by definition too old to
send a NAK. Infer it without new opcodes: `CMD_PING` and `CMD_GET_TYPE` are
universal, so *ping answers but set_session times out* ⇒ report "old firmware", not
"absent".

**The idle heartbeat depends on §7.** Under a blocking caller it reintroduces the
4-second stall.

### 8.3 Bus alarm

Do **not** wait for the refactor — [bus_alarm.md](bus_alarm.md) is overwhelmingly
Core-1 and node-side. Three cheap hooks:

1. **Add `ALARM_BUS = 5`** to the enum (currently stops at `ALARM_HOMING_FAIL = 4`)
   and make `reconcileValidity()` call `originInvalidateAll()` for it, exactly as
   it does for `ALARM_ESTOP` and `ALARM_SOFT_LIMIT` at `control_plane.cpp:240`. Add
   the matching entry to `web/src/wire/format/names.ts` or it prints `ALARM(5)`.
   **Why:** the premise of §0 is that the node refused steps while the Pico kept
   counting, so position has diverged **by construction**.

2. **Put the loopback ring in `core1/bus/loopback.h`** (header-only, inlined) — not
   inline in the emit loop. Pair `lbReset()` with the four existing `flushRX()`
   sites: `core1.cpp:353`, `:541`, `:552`, `:719`.
   **Why:** §3.1's code goes right after `core1.cpp:169`, inside
   `__time_critical_func`, in a loop with a budgeted ~5000 cycles per step that
   **currently never drains RX at all**. §4.1 also needs a second variant for the
   vacuum's echo check.

3. **Add the §7 fault counter before any alarm logic exists** — a
   `volatile uint32_t` in `ipc/shared_state.h`, surfaced via `getstate`
   (`cmd/query.cpp`).
   **Why:** not polish. §11 step 3 makes the false-positive rate over an idle hour
   *the acceptance criterion for the whole design*. Bring-up needs somewhere to
   report into on day one.

Do not scaffold §10's idle-case break detector (new PIO SM, explicitly deferred),
§3.2/§4.1 (pure node-side AVR register work), or the §7 self-test opcode (premature
before the mechanism works).

### 8.4 Node soft reset

`SET_SESSION`'s claim-and-safe (§8.2 step 4) **is** a node soft reset with a token
attached, and it supersedes §2.2's sweep — closing the gap positively rather than
by remembering to sweep.

Add a separate `CMD_RESET` eventually for bench recovery without claiming, and to
make the host-facing `reset` verb mean "reset the machine" rather than "reset the
Pico." Additive; `SET_SESSION` does the safety-critical half.

---

## 9. Target tree

After Stages 1–4:

```
src/rp2350/
├── main.cpp                      66    unchanged
├── board.h                       ~10   NEW (pins)
├── ipc/                                NEW — everything crossing the core boundary
│   ├── core1_rpc.{h,cpp}        ~90/180   channel 1
│   └── shared_state.h           ~200      channels 2 + 3
├── config/
│   └── config_store.{h,cpp}      68/149   unchanged
├── core0/
│   ├── core0.cpp                 139   unchanged
│   ├── usb_protocol.h           ~170  NEW
│   ├── data_plane.{h,cpp}        27/323  unchanged
│   ├── status.{h,cpp}            13/41   unchanged
│   ├── control_plane.{h,cpp}     19/~170  dispatch table only (was 1034)
│   ├── position.{h,cpp}          ~60/200  NEW
│   └── cmd/
│       ├── table.h              ~40   NEW
│       ├── parse.{h,cpp}        ~25/70   NEW
│       ├── query.cpp            ~180  NEW
│       ├── lifecycle.cpp        ~110  NEW
│       ├── periph.cpp           ~90   NEW
│       └── axis.cpp             ~280  NEW
└── core1/
    ├── core1.cpp                ~120  setup1/loop1, top loop, flash park (was 746)
    ├── rpc_server.cpp           ~180  NEW — the RPC server side
    ├── motion_limits.h          ~45   NEW (delete when config lands)
    ├── emit/
    │   ├── microsegment.cpp     ~256  NEW
    │   └── debug_step.cpp       ~50   NEW
    └── bus/
        ├── RS485Bus.{h,cpp}      42/50   byte layer, unchanged
        ├── packet.{h,cpp}       ~80   NEW — frame layer
        ├── loopback.h                 NEW, only if §8.3 is built
        └── uart_9bit.pio(.h)          unchanged
```

`shared.h` is gone. Line counts for new files are estimates.

---

## 10. Decisions already settled

Do not relitigate these.

| Question | Decision | Where |
|---|---|---|
| BSP, or something else? | Ports-and-adapters. The host already names it — `web/src/wire/link/transport.ts` defines an "environment-agnostic I/O contract" with three backends. | §0 |
| Name the module `link`? | No — collides with the host's `wire/link/`, and Core 0 is always the initiator. `core1_rpc` names the asymmetry. | §3 |
| Keep `shared.h`? | No, and no umbrella shim. Each block moves to its final home in §3.1. | §3.1 |
| Edit in place, or build beside? | Build beside, in final form. The strangler fallback is deleted, never rewritten. | §1 |
| Port the FIFO protocol, then swap to `queue_t`? | No — that is writing the transport twice. Go straight to `queue_t`, async-capable. | §3.3 |
| Split axis map from datum model? | No. `slotAdoptStatus` is their join point. | §5 |
| Slot map cross-core for `bus_alarm` §4.4? | No — hand Core 1 an expected byte mask at probe-arm time. | §5 |
| FreeRTOS instead? | No. Core 1 is a cycle-counted busy-wait with a ~5000-cycle step budget (`core1.cpp:135`) and cannot host a scheduler; there are no tasks to schedule on Core 0; an RTOS wants the hardware FIFO exclusively. | §3.3 |
| Add a `gate` field to `struct Cmd`? | No. `stateIs(...)` at the handler top already works. The gate field bought tidiness and dragged in `STATE_CONFIG`, a wire change and a `web/src` migration. §6's table fixes the real bugs (prefix ordering, `argAfter` offsets) without it. | §7 |
| Promote `ALARM_CONFIG` to a state? | No. `shared.h:273` — reason codes exist so no sub-states are needed. Only needed to complete a gate table that no longer exists. | §7 |
| New state for a node fault? | No. `ALARM_NODE_FAULT` is an `AlarmReason`; no gate reads a reason, so `ALARM`'s existing gates apply unchanged. | §7.4 |
| Guard async commands with an `alarmEpoch`? | No. One relay in flight at a time makes the existing `alarmAtEntry` compare (`:906`/`:937`) sufficient, and makes reply ordering trivial. | §7.3 |
| Widen the gate on `pingnode` / `nodepos` / relayed `enable`/`disable`? | No — reversed. Their gate had a Core 0 half (a transport artifact, removed by Stage 1) and a Core 1 half (an RS485 exchange inside the step budget, unchanged). All nine gated commands share the second. Every gate stays. | §7 |
| Fix the underfed-job gate hole in Stage 5? | No. `machineState` leaves `RUNNING` when the ring drains, so the gate admits commands in exactly the window it claims to cover. The right predicate is a motion-side change and needs a measurement; the firmware stays ignorant of underfed jobs until then. | §7.2 |
| Add `STATE_HOMING`? | Nothing to add — already allocated and parsed on both sides (`shared.h:270`, `status.ts:34`). Reserved-but-understood; costs no wire change whenever auto-home lands. | §7 |
| Where does `fw id` live? | `SET_SESSION` ack, not the periodic status head. Pays for a 16-bit token. | §8.2 |

## 11. Open

- **Is there a spare conductor?** ([bus_alarm.md](bus_alarm.md) §9, §12) A wired-OR
  ALARM line is strictly better than readback-compare and would delete §3 entirely.
  **Check before building any of §8.3 step 2.**
- **Async reply ordering** under §7 — how `handleCommand` preserves
  one-line-per-command once it is no longer synchronous. Decide the shape while
  writing §3.3's API, since that is what has to support it. **§7.3 closes this by
  keeping one relay in flight at a time — one outstanding command, one reply.
  Confirm that constraint holds before designing anything more elaborate.**
- **What predicate should the bus-command gate use?** (§7.2) `stateIs(IDLE,
  PAUSED, ALARM)` keys on `machineState`, which drops out of `RUNNING` whenever
  the ring drains — so an underfed job admits every gated command in precisely
  the window the gate exists to close. The question is "is a job in flight",
  which nothing represents: `jobActive` means *suspended*. Candidates are a
  streaming flag Core 0 sets at ingest, or `queuedUsIn != queuedUsOut`. **First
  measure whether the ring underruns mid-job at working feed rates** — that
  decides whether this is a live hole or a theoretical one. Until it is answered
  the firmware is deliberately ignorant of underfed jobs.
- **Is the mid-cut mark real?** (§7.1) Every gate here survives on the strength
  of a comment nobody has measured. Scope a hardware test: toggle `knife_blower`
  mid-cut with the gate lifted and inspect the material. If no mark appears at
  working step rates, all nine commands lose their gate and the question above
  dissolves with them. If it does, the fix is Core-1 side — defer the exchange to
  a segment boundary — not a state gate.
- **Error-string normalisation** (`err usage` vs `err bad_node`) — deferred out of
  §6 deliberately; needs the host in step.
- **Immediate or latched alarm on a refused step** — [bus_alarm.md](bus_alarm.md)
  §12, undecided.
