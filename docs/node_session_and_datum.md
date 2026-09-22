# Node Sessions & the Position Datum

**Status:** design, not implemented
**Companion to:** `engage_and_axis_map.md` (slots), `wire_protocol.md` (`nodestat`)

Two changes that share one reply and one question — *when may the Pico trust a
position?*

---

## 1. The problem

`machinePos[4]` is indexed by **slot**, not by node. `axis_map` rewrites the
slot→node binding without touching it, so:

```
axis_map 1 2 4 3   →   axis_map 1 2 6 5
```

leaves `machinePos[2..3]` holding nodes 4 and 3's counts, now labelled 6 and 5,
with `axes_homed` still asserting Z/A are good. Nodes 4 and 3 keep their true
positions; the Pico has no register for them and forgets. Next Z/A move is wrong,
silently.

Note the node data is fine — a disengaged stepper can't move *and* stops counting
(`if (slot == SLOT_NONE) return;`), so a parked node's counter stays valid. The
Pico just never reads it.

## 2. Move the datum into the node frame

A node's `absolutePosition` is zeroed at **node boot**, not at machine datum, so
seeding `machinePos` from it directly is meaningless. Keep the offset instead:

- Pico holds `int32_t nodeOrigin[BUS_ADDR_MAX+1]` + a per-**node-id** homed bit.
- `setorigin` records `nodeOrigin[node] = <node's counter>` instead of zeroing
  `machinePos[slot]`.
- On engage, read the node's counter and seed
  `machinePos[slot] = nodePos - nodeOrigin[node]`.
- `axes_homed` becomes **derived**: slot `i` is homed iff `slotNode[i]` has a
  valid origin.

Dead-reckoning then continues unchanged until the next `axis_map`. Park a head,
run another, come back — the datum survived, because it lived with the node.

Falls out for free: `emitDebugSteps` no longer needs `axes_homed = 0`. Those
steps *are* tracked — by the node.

## 3. Sessions: the Pico assigns the token

A node-side boot counter would need EEPROM (wear; RAM won't survive a brownout,
which is the case that matters). Invert it: `CMD_SET_SESSION` writes a
Pico-chosen nonce into node RAM, and `nodestat` echoes it back. Reboot → RAM
clears → token reads 0 → mismatch. No persistence needed.

Two properties worth naming:

- A Pico reboot issues fresh nonces, so it cannot inherit stale trust.
- `token == 0` means *never configured by me* — which is the detector for the
  connect-time unknown-stale-engagement gap left open in
  `engage_and_axis_map.md`.

### What it catches

| | |
|---|---|
| **Silent mid-job step loss** | A brownout reboots a node into `SLOT_NONE` + `streamEnabled=false`; it ignores the rest of the job while the Pico dead-reckons and `getpos` reads perfectly. Today nothing detects this. |
| **Why it reset** | `RSTCTRL.RSTFR` is one free byte: BOD → power/motor transient, WDT → firmware hang, UPDI → someone reflashed it. |
| **Firmware identity** | A build id would have made `err node 1 timeout` (node running pre-ENGAGE firmware) a one-line read. |
| **Hot-swapped heads** | Distinguishes "node 4 parked, datum intact" from "different physical head at that address" — opposite situations, otherwise identical on the wire. |
| **Job integrity gate** | Snapshot tokens at job start, re-check at end; any change voids the run's position claims. |

## 4. Wire shape

Extends `nodestat`'s **generic head** — no node type is touched:

```
[node_type][flags][session token][reset cause][fw id]  + type-specific tail
```

## 5. Open

- **Token width** — 1 byte detects reboots but collides ~1/256 on re-issue; a
  16-bit Pico-side monotonic counter never collides within a session.
- **Poll cadence** — mandatory at job start and job end; otherwise fold into an
  idle heartbeat. Keep it out of `RUNNING`: it competes with the stream on the
  same bus, so accept end-of-job detection there.
- **Node reboot mid-`axis_map`** — engage ACKs, then the node resets before the
  status read. Re-read after engage, or accept the next heartbeat catching it.

---

## 6. Delayed start (Pico powered before the nodes)

Mostly safe already, by two existing choices:

- The `ALARM_CONFIG` gate refuses all motion until a map commits, so "booted
  ahead of the nodes" is a legal resting state.
- A node boots **disengaged and de-energised**, so one arriving late — even
  mid-stream — ignores every stream byte until an explicit `CMD_ENGAGE`. A
  late arrival cannot inject motion. Treat this as a guarantee, not an accident.

Sessions extend cleanly: a late node reports `token == 0`, so a heartbeat
discovers it without anything special.

### The gap: partial apply

`axis_map` disengages the old set and engages the new one, then returns on the
first timeout with `slotNode[]` **unchanged** — so the bus is partly reconfigured
while the map still describes the old world. That is the divergence
always-re-engage exists to prevent, and with delayed start it is the *usual*
path, not a rare one. Options:

- Commit the map to what actually ACKed (partial map + `ALARM_CONFIG` held).
- Roll back: re-engage the previous set before returning.
- Retry/poll until the full set answers, staying in `ALARM_CONFIG` meanwhile.

The third is closest to what a delayed-start machine actually wants, and needs a
discovery affordance — today the operator must manually re-issue `axis_map`.

### Minor

- Between node reset and `node_setup`'s first `HAL_MOTOR_DISABLE()`, EN is an
  undriven input. Whether the driver is briefly energised is a board-level
  pull question, not a firmware one — worth confirming on hardware.
- Motor-supply power-up can brown out already-booted nodes. Staggered power
  makes this likelier; session tokens are what catch it.

---

## 7. Ownership: the token as a capability

Stronger form of §3 — the Pico *owns* a node's boot. Commands carry the token;
a node NAKs on mismatch. Scope: this is anti-**confusion** (stale master,
reflashed host, leftover binding), not security. One master, one byte, CRC8 —
do not describe it as authentication.

### Stream inherits, it does not carry

A stream byte is 8 bits with all 8 allocated to four slots; there is no room for
a token and never will be. It needs none: `CMD_ENGAGE` is the token-checked
command that **grants** stream access, so the slot binding is the capability
handle. No token → no engage → no stream. This makes the disengage convention an
enforced invariant rather than etiquette, at zero per-byte cost.

### Open vs owned

Requiring the token everywhere deadlocks discovery — you could not identify an
unowned node or diagnose a mismatch. The clean line is identity and reads are open; 
effects are owned:

- **Open** — `PING`, `GET_TYPE`, `NODE_STATUS`, `GET_POS`, `SET_SESSION`.
  Identity and reads.
- **Owned** — `ENGAGE`, `ENABLE`/`DISABLE`, `LASER`, `SERVO_SET`, knife verbs.
  Anything with a physical effect.

### Takeover always succeeds, and self-safes

If a node refused `SET_SESSION` without the prior token, a Pico reboot would
brick every node until power-cycled. So accept any claim — and make claiming
**reset the node to safe state**: disengage, de-energise, laser off.

That closes the stale-engagement gap *positively* instead of by detection: claim
→ node self-safes → engage deliberately. It also gives delayed start (§6) one
boot sequence: poll `PING` until answer → `SET_SESSION` (claims + safes) →
`ENGAGE`.

### Prerequisite: a real NAK

There is no NAK opcode. `CMD_LASER`'s "NAK elsewhere" is aspirational — an
unhandled command is silently dropped and the master times out. Bad-token would
then be indistinguishable from node-absent, which is the ambiguity that made a
pre-ENGAGE-firmware node present as `err node 1 timeout`. Needs
`CMD_NAK [reason]` with `BAD_TOKEN` / `UNSUPPORTED` before any of this is
diagnosable.

### Cost

- +1 byte on every command packet. Header (`[ID][CMD][TOKEN][LEN]…`) is uniform
  but re-indexes every payload; payload-prefix is less invasive but muddies
  length semantics. Header, given the check is central.
- The gate lives in `routeCommand` (`dispatch.cpp`) — one predicate over the
  open-command set, checked before any side effect. **No node type changes.**
- Breaks wire compat with un-updated nodes. Acceptable, and the `fw id` of §4 is
  what makes that failure legible rather than a mystery timeout.
