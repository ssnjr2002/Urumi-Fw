# Proposal (not scheduled): one ingest funnel per core for node-reported state

Status: **design settled, nothing built.** Companion to
`node_frame_ownership_migration.md`, which asked whether `nodeOrigin`/`nodeHomed`
should move to Core 1 and answered no. This doc came from re-asking that question
from the other end — *who folds a node's reply into machine state* — and reaches
a different, smaller change that does not move any ownership at all.

Written down because the reasoning took a long path with several plausible wrong
turns on it (§6), and none of them are obviously wrong from a standing start.

## 1. The observation

Core 1 hands Core 0 the **whole** node reply: `serveNodeCmd` ends with
`replyWith(req, RPC_OK, buf, rxLen)` (`core1/rpc_server.cpp`), payload verbatim.
Core 0's caller then decodes a `NodeStatus` and reads the one field it asked
about. Everything else in that reply is discarded at the call site.

That surplus is real. A `CMD_ENGAGE` ack carries the node's enable flag, its
datum witness and its step counter; a handler that wanted the counter throws the
other two away, and the next command re-asks for what the bus already said.

So: put a funnel in front of the reply. It syncs what is tracked, then returns
the payload for the caller to use as before.

## 2. Two funnels, split by provenance

Not by field, and not by core-as-owner. The split is **did Core 0 ask for this
transaction**:

| | Core 1 funnel | Core 0 funnel |
|---|---|---|
| hook | `serveNodeCmd`, unsolicited sweeps | `rpcCall()` (`ipc/core1_rpc.cpp:64`) |
| covers | transactions nobody requested | every transaction Core 0 requested |
| input | the reply bytes, before `replyWith` | `req` + `rep`, after `rpcCall` returns |
| writes | `nodeEnabled` only | the rest of the node frame |

`rpcCall` is the single choke point for the solicited half — every wrapper
(`rpcNodeCmd`, `rpcNodeStatus`, `rpcSwitchGet`, `rpcHome`, …) goes through it,
and it holds both the request and the reply, which is what a funnel needs: the
payload cannot be interpreted without the `cmd` that produced it.

```c
// ipc/core1_rpc.cpp, inside rpcCall(), after the reply lands
if (r == RPC_OK) ingestReply(&r, out);   // Core-0-owned fields only
return r;
```

Structurally this mirrors `noteEnabled` sitting on `serveNodeCmd` — same idea,
other end of the wire. `noteEnabled` **is** the Core 1 funnel already, at n=1.

### 2.1 Single writer is unchanged

Each funnel writes only fields its core already owns. The funnel does not make
dual-writer safe and is not an attempt to; it makes the single-writer rule
*locally checkable* — one function per core to audit instead of a property that
has to hold across every call site.

### 2.2 Per-command branches

The funnel dispatches on `cmd`, because reply shapes differ: `rpcSwitchGet`
answers with a bare level byte, not a `NodeStatus`. `answersWithStatus()`
(`core1/rpc_server.cpp:45`) already enumerates the status-bearing set —
`CMD_NODE_STATUS`, `CMD_DATUM_SET`, `CMD_ENGAGE`, `CMD_HOME_LEG` — and Core 0's
funnel mirrors that shape.

Each branch knows statically what its reply carries, which is the point: a
`CMD_ENGAGE` wrapper knows the ack reported enable state, so it can refresh the
slot-frame view right there (§4) instead of waiting for a periodic recompute.

### 2.3 Only `RPC_OK` writes

Timeouts and NAKs leave state alone rather than guessing in either direction.
Same doctrine `noteEnabled` documents, and the same reason `busDisableAll` keeps
the bit of a node that did not answer.

## 3. What may be synced

The partition is not "tracked / untracked" but **what makes a stored value stop
being true**:

| class | fields | sync? |
|---|---|---|
| immutable per node | `homingKind`, `stepsPerRev`, `fwId` | yes — never stale |
| event-lifetimed | `flags` (`NODE_FLAG_ENABLED`, `NODE_FLAG_DATUM`), switch level, `session` | yes — changes only when something observable changes it |
| continuously stale | `pos` | **no** |

`pos` is the trap: the field most tempting to fold in and the one least worth
storing. It is true only at the instant of that reply, and an axis in motion
makes it a lie with no marker saying so. For a **bound** node `machinePos[]` is
already the live truth and strictly better. For an **unbound** node the counter
is static — which is exactly why `parkPos[]` exists, as a one-shot memory rather
than a mirror.

Dropping `pos` also keeps the whole scheme lock-free: what remains is bitmasks
and single bytes, every one of which is a naturally-aligned atomic access.

### 3.1 Derived state is not synced

The funnels write **node-frame** facts only. `nodeOrigin[n] = nodePos -
machineSteps` fuses a wire fact with an operator argument that never travels on
the bus, so `setorigin` keeps its handler-level write. Unchanged from
`node_frame_ownership_migration.md`, and for the same reason.

## 4. The projection stops being periodic

`axes_enabled` is a view of `nodeEnabled` through the axis map. Today
`reconcileValidity()` recomputes it every Core 0 loop pass. With per-command
branches it can be refreshed at the moments node truth or the map can change:

```c
// core0/position.cpp
void projectEnabled(void) {
    uint8_t e = 0;
    for (uint8_t s = 0; s < MOTION_SLOTS; s++) {
        const uint8_t n = slotNodeAt(s);
        if (n != SLOT_NONE && (nodeEnabled & (1u << n))) e |= (1 << s);
    }
    axes_enabled = e;
}
```

Complete trigger set: status-bearing replies, `slotBind`, `slotUnbind`,
soft-reset completion, and the alarm edge (§4.2).

### 4.1 The RPC round trip is the happens-before edge

Core 0's funnel reads `nodeEnabled`, which Core 1 wrote. That ordering is
guaranteed with no barrier and no generation counter:

```
core1: noteEnabled(...)         rpc_server.cpp:173   ← nodeEnabled written
core1: replyWith(...)           rpc_server.cpp:174
core0: rpcCall returns                               ← blocked until reply lands
core0: ingestReply(...)                              ← reads nodeEnabled
```

`rpcServerReply`/`rpcPoll` are `queue_try_add`/`queue_try_remove` on
`pico/util/queue.h`, which take a hardware spinlock internally: `spin_lock_blocking`
fences on acquire, `spin_unlock` does `__mem_fence_release()`. Full release/acquire
pair on every reply.

**Rule worth keeping:** anything travelling through the RPC queue is ordered for
free. The existing `__dmb`s appear only around **raw shared globals**
(`machineState`/`alarmReason`, the reset handshake flags) that bypass the queue.
Do not add fences to the queued path.

### 4.2 `reconcileValidity` shrinks but stays in the loop

```c
void reconcileValidity(void) {
    uint8_t st = machineState;
    __dmb();                       // raw globals, not queued — still required
    uint8_t ar = alarmReason;

    static bool originLatched = false;
    bool originActive = (st == STATE_ESTOP || ar == ALARM_ESTOP ||
                         ar == ALARM_SOFT_LIMIT);
    if (originActive && !originLatched) {
        originInvalidateAll();
        projectEnabled();          // the sweep changed nodeEnabled with no reply
        originLatched = true;
    } else if (!originActive) {
        originLatched = false;
    }
}
```

It stays in the loop, and that part is irreducible: the estop sweep is the one
state change with **no reply to hang a funnel off**, so a level poll is the only
way Core 0 learns of it. The win is that the 4-slot loop leaves the tick; what
remains is two loads, a fence and a compare.

`projectEnabled()` on the alarm edge must **recompute, not zero** —
`busDisableAll` deliberately keeps the bit of a node that did not answer
(`core1/bus/packet.cpp`).

Consider renaming. With the projection gone it does exactly one thing: fold in
Core 1's asynchronous alarm signal. `reconcileValidity` was named for the era
when it reconciled two frames.

The `axes_homed` half is already event-driven (`slotAdoptStatus`,
`originInvalidate`) and needs no change — a fair sign this is the shape the file
was converging on anyway.

## 5. Why `nodeEnabled` cannot move to Core 0

The tempting simplification is: Core 0 requests every bus transaction and gets
every reply, so let Core 0 own everything and delete the Core 1 funnel. It does
not work, for two independent reasons.

**Core 1 acts without being asked.** `busDisableAll()` runs from the estop path
(`core1/core1.cpp`, before `STATE_ALARM` is published) and the soft-reset park.
There is no reply to ingest because nobody asked. Making Core 0 sole writer means
Core 0 must learn a fact nobody sent it.

**`nodeEnabled` and `axes_enabled` are not the same information.**
`axes_enabled` is 4 motion slots; `nodeEnabled` is bus ids 1..8. The difference
is the **peripherals** — a vacuum or knife node holds no motion slot, so
`slotNode[]` cannot reach it, and those are exactly the nodes that must not keep
running after an estop. It is also node-framed so it survives a rebind, the same
reason `nodeLatched` is (`core0/position.h`).

### 5.1 Why Core 1 must own the sweep

Asked directly: why can Core 0 not command the sweep like every other bus
command?

1. **The poison pill** is detected by Core 1 mid-emit. Routing through Core 0
   adds two hops gated by a loop cadence nothing bounds.
2. **Core 0 can be stuck mid-RPC.** `rpcPost` admits one reply-bearing
   transaction at a time and `rpcCall` spins to `RPC_CALL_TIMEOUT_MS`, so an
   estop arriving mid-call cannot be relayed until that call resolves. The
   likeliest cause of a slow RPC is a node that stopped answering — the
   mechanism would fail hardest in the case it exists for.
3. **The ordering invariant** ("once you observe ALARM, everything on the bus is
   already parked") would either break, or require Core 1 to block on Core 0's
   liveness inside the estop path.
4. **Soft reset** sweeps while Core 0 is tearing down and both queues are
   drained. There is no RPC to issue it with.

Underneath all four: Core 1 owns the UART. The RPC exists *because* Core 0
cannot touch the bus.

Core 1 must sweep → Core 1 must record what it swept → the mask is Core-1-written.
The rest of the design follows from that one constraint.

## 6. Rejected alternatives

Each of these was proposed and looked reasonable. Recorded so they are not
re-derived.

**Delete-after-read mailbox on Core 1.** Consume-once entries so a fact can never
be read twice, making stale reads structurally impossible. Rejected because the
surplus is not discarded by Core 1 at all — Core 0 already receives the whole
payload (§1) — so the mailbox would ship a second copy of bytes Core 0 has.
Destructive reads also make consumers race each other, and "absent" then
conflates *never reported* with *already consumed*.

**SPSC queue of unsolicited pseudo-replies.** Coherent, and it would salvage the
eight payloads `busDisableAll` currently drops (it passes `nullptr` as the
receive buffer). Rejected because its only justification was making Core 0 sole
writer, which §5 rules out anyway. It would also downgrade the ALARM ordering
invariant from a mechanical guarantee to a drain-ordering convention.

**A `nodeGen` generation counter / seqlock for the ingest.** Solves a cross-core
visibility problem that §4.1 shows does not exist on this path. Worth revisiting
**only** if the funnel ever grows a multi-word per-node record (`{int32 pos;
uint8 flags; uint16 session;}`) that must be read as one coherent unit — a single
writer is not enough there, and the right answer is a seqlock, not a lock.

**Inferring "position died" from `nodeEnabled`.** Three variants were tried:
*changed*, *`== 0`*, and *flagged by `busDisableAll`*. All are proxies for a fact
they only correlate with:

- an ordinary `axes_enable off` changes the mask but must not wipe every origin;
- a soft-limit trip destroys position and does **not** sweep the bus, so the mask
  never changes — a silent miss, latent until soft limits are raised;
- `nodeEnabled == 0` is never reached when a node fails to answer, so position
  would survive an estop *precisely when a node is misbehaving*;
- flagging inside `busDisableAll` delays invalidation until the sweep completes
  (up to 8 × `RESPONSE_TIMEOUT_MS`), inverting the documented ordering —
  "position dies the instant motion stops abruptly, before the bus sweep."

`nodeEnabled` records what the bus confirmed; "position died" states why motion
stopped. They overlap on the happy path and diverge on the faulty-node path,
which is the path both exist to survive.

**A shared `positionDied` bool both cores write.** `__dmb` does not help: it
orders one core's accesses, and provides no atomicity or mutual exclusion. Core 1
setting between Core 0's read and clear is a lost update — the same shape as the
historical `axes_enabled` bug. Core 0 also has no need to write it: a flag exists
to carry a fact *across* the boundary, and Core 0 telling itself something just
calls `originInvalidateAll()` directly, as `probe.cpp` already does.

## 7. Open items

**`originKillGen`, when soft limits land.** A monotonic `volatile uint8_t` that
Core 1 increments before any path that ends motion abruptly, with Core 0 keeping
its own `lastSeen` — each side writes only its own variable, so no barrier and no
lost update. `uint8_t` suffices: Core 0 compares for inequality and never
subtracts, so wrap is harmless (missing an edge would need 256 estops inside one
loop pass). Contrast `queuedUsIn/Out`, which are `u32` because they are
arithmetic. Exclude it from the soft-reset wipe, or Core 0's `lastSeen` desyncs.

Not worth it today — estop is the only live path, and one condition stated at the
reader is simpler than a global plus a rule. **The trigger is the second real
writer.** Both conditions already belong to Core 1: soft-limit detection is
Core 1's (`core1/emit/microsegment.cpp` already writes `ALARM_SOFT_LIMIT` as a
harness), and Core 0's `stop` is transitively Core 1's, since Core 0's write is a
request Core 1 enacts.

The pressure is already visible: keying invalidation on the alarm *reason* fuses
two unrelated decisions, and the fusion has leaked into modules that have nothing
to do with it — `probeCauseVoidsDatum` (`ipc/core1_rpc.h`) exists, and
`probe.cpp` carries a comment warning that a probe-fail alarm "must not use
either of those reasons". `ALARM_PROBE_FAIL` was partly minted to escape it.
A third instance of that workaround is the signal to switch.

**`alarmReason` is already a dual-writer field.** Core 1 writes it
(`core1/core1.cpp`, `emit/microsegment.cpp`); Core 0 writes it in `homing.cpp`,
`probe.cpp`, `cmd/axis.cpp`, `core0.cpp`. Safe today only because the writes are
disjoint in time — each core writes while owning the transition into its own
alarm — but that is convention, not structure, and nothing in the header says so.
Same shape as the bug that moved `nodeEnabled`. Not urgent; worth a header note.

**`MSEG_FLAG_ESTOP` is dead and has teeth.** No host sends it (nothing in
`web/src`), and it is handled at the head of the ring
(`core1/emit/microsegment.cpp`), so it fires only when the emitter *reaches* it —
behind up to 511 queued segments. It is therefore an ordered halt, not an
emergency stop; the out-of-band `stop` command is the real one. Unlike
`MSEG_FLAG_PATH_END` it **is** in `MSEG_FLAG_WIRE_MASK`, so a host that sets bit
1 by accident gets ALARM and a destroyed datum. Options: delete it, demote it out
of `WIRE_MASK` as `PATH_END` was, or keep and rename it if "abort at a known path
position, deliberately voiding position" is wanted — `MSEG_FLAG_PAUSE` already
covers the resumable version. Check whether it predates `MSEG_FLAG_PAUSE` and
`ABORT` before deciding.

**Stale comment.** `core1/core1.cpp` still says Core 0 clears `axes_enabled`
"keyed on exactly that transition (ALARM + ALARM_ESTOP)". `core0/position.cpp`
now projects it unconditionally from `nodeEnabled` and says so explicitly. Same
conclusion, wrong mechanism described.

## 8. Why do it at all

Weakest part of the case, stated honestly: on day one `ingestReply` is
`noteEnabled` renamed, with nothing new folded in, because no consumer wants the
other fields yet. What it buys is a named place for the next one to go, and the
removal of a per-tick recompute.

Unlike the migration in `node_frame_ownership_migration.md` it moves no
ownership, widens no RPC struct, and adds no cross-core machinery — it is a
refactor of an existing function plus one hook in `rpcCall`. It can land empty
and grow a field at a time.

## 9. When to revisit

- A second Core-1 path that ends motion abruptly → build `originKillGen` (§7).
- A multi-word per-node record in the funnel → seqlock, not single-writer (§6).
- A Core-1 consumer that needs node state it did not just ask for (the probe
  supervisor in `bus_alarm.md` §4.4 is the candidate) → that, not symmetry, is
  what would justify Core 1 keeping more than `nodeEnabled`.
