# Proposal (not scheduled): move `nodeOrigin`/`nodeHomed`/`parkPos` ownership to Core 1

Status: **deferred, no known gap justifies it today**. Written down so the
reasoning isn't re-derived from scratch next time it comes up.

## Background

`nodeEnabled` was moved from Core 0 to Core 1 (see `ipc/shared_state.h`) because
Core 1's own ESTOP sweep (`busDisableAll`, `core1/bus/packet.cpp`) writes it
*asynchronously*, outside any RPC Core 0 initiated — the two cores' writes could
race. That's a real hazard fixed by a real inversion.

The question raised in review: does the same inversion make sense for
`nodeOrigin[]` / `nodeHomed` (`core0/position.cpp`) and `parkPos[]` / `parkSeen`?

## What's actually true of these fields

Unlike `nodeEnabled`, nothing writes them asynchronously — every write today
happens inside a Core-0 command handler, synchronously, after an RPC call
returns. There is no race to fix. The motivation would be purely structural:
"Core 1 already sees the bus reply that proves this fact, why does Core 0 have
to relay it."

Checked against that framing, most (not all) of the current Core-0 write sites
line up with a bus reply Core 1's `rpc_server.cpp` already decodes:

| Site | Trigger | Bus-transaction-backed? |
|---|---|---|
| `setorigin` (`cmd/axis.cpp:295-323`) | `CMD_DATUM_SET` confirmed | yes |
| `cmdDisable` (`cmd/axis.cpp:124`) | `CMD_DISABLE` confirmed | yes — same reply `noteEnabled` already reads |
| `axes_enable off` (`cmd/axis.cpp:72`) | per-node `CMD_DISABLE` | yes, same as above |
| `bus_enable off` (`cmd/axis.cpp:92`) | broadcast `CMD_DISABLE` | yes, same as above |
| `parkMoved` (`cmd/axis.cpp:212`) | `CMD_ENGAGE` reply vs. remembered counter | yes, but needs Core 1 to also keep `parkPos[]`/`parkSeen` |
| `homingRelease` (`homing.cpp:63`) | `CMD_HOME` completing | yes, same funnel as the others |

One blocker found while scoping: `nodeOrigin[n] = nodePos - machineSteps`
combines a bus fact (`nodePos`) with host intent (`machineSteps`, the
operator's `setorigin` argument). Core 1 can't compute this from the wire
reply alone. It's solvable — `req->args[]` already carries values that never
hit the wire (e.g. `CMD_ENGAGE`'s slot-index arg), so `machineSteps` could ride
the same channel — but it means the migration isn't a pure "read what's
already there," it requires widening the RPC request shape for this one call.

## Why not do it

- **No bug it fixes.** `nodeEnabled`'s move closed the `getstate` showing
  `enabled=0x00` after ESTOP-recovery bug. Nothing analogous exists here — no
  bench report, no stale-read complaint against `nodeOrigin`/`nodeHomed`.
- **All five sites, not one.** Doing only `setorigin` leaves `nodeHomed` a
  real dual-writer (Core 1 sets it, Core 0 still clears it from four other
  places) — worse than today, since dual-writer is exactly what the
  single-writer discipline exists to prevent. The honest scope is all five
  sites plus giving Core 1 its own `parkPos[]`/`parkSeen`, together.
- **Touches the RPC request shape.** The `machineSteps` argument needs
  `core1_rpc.h`'s request struct widened for one call, which every other RPC
  call site is indifferent to but still has to compile against.
- **The one thing it would buy** is Core 0 no longer manually relaying facts
  Core 1 already has evidence for — a satisfying symmetry with `nodeEnabled`,
  but symmetry alone isn't a reason to touch code that isn't broken.

## A different cut at the same question

`node_state_ingest.md` re-asks this from the other end -- not *who owns the
field* but *who folds a reply into it* -- and lands on a smaller change that
moves no ownership: one ingest funnel per core, split by whether Core 0 asked
for the transaction. It reaches the same verdict about the fields below (they
stay on Core 0) by a different route, and it records why `nodeEnabled` cannot
move the other way either.

## When to revisit

If a bench report ever shows `nodeHomed`/`getpos` disagreeing with what a node
actually holds (the `nodeEnabled` bug's shape, but for the datum instead of
enable state), this doc has the sketch already done — start from the table
above rather than re-deriving it.
