#pragma once
#include <stdint.h>
#include "../ipc/core1_rpc.h"    // NodeStatus

// position.h — the axis map and the node-frame position datum.
//
// One module, not two, and deliberately so. "Which bus id occupies stream slot
// i" and "where is that node, in whose frame" are the same fact stated twice —
// which is exactly what the node-frame design is FOR (docs/node_session_and_datum.md
// §2, docs/engage_and_axis_map.md §5). Validity is a conjunction across three
// ownership domains:
//
//   Core-0 statics   slotNode[], nodeOrigin[], nodeHomed, parkPos[], parkSeen
//   Cross-core       machinePos[] (Core 1 writes it), axes_homed, axes_enabled
//   Node-reported    NODE_FLAG_DATUM, the node's own step counter
//
// slotAdoptStatus() is the join of all three. Cutting between "axis map" and
// "datum model" would put the two halves of that conjunction in different files.
//
// The state itself stays private to position.cpp. Callers get named operations
// instead of the arrays, because every historical bug in this area was a caller
// updating one frame and forgetting the other -- clearing axes_homed but leaving
// nodeOrigin, so the next axis_map cheerfully resurrected a dead datum. There is
// no way to express that mistake through this header.
//
// Core-0-private by design. Core 1 never sees the map; when the probe supervisor
// lands (docs/bus_alarm.md §4.4) it is handed an expected byte mask at arm time
// rather than the map itself.

#define SLOT_NONE     0xFF
#define MOTION_SLOTS  4     // stream-byte motion slots (X/Y/Z/A)

// ─── Axis map ─────────────────────────────────────────────────────────────────

// All slots unbound. Boot state: the machine sits in ALARM_CONFIG until an
// axis_map commits a binding.
void axisMapReset(void);

// The bus id bound to slot `s`, or SLOT_NONE. Slot indices are 0..MOTION_SLOTS-1.
uint8_t slotNodeAt(uint8_t s);

// The slot holding bus id `n`, or SLOT_NONE if `n` is not an axis node.
uint8_t nodeSlot(uint8_t n);
static inline bool node_isAxis(uint8_t n) { return nodeSlot(n) != SLOT_NONE; }

// Bind slot `s` to node `n` and adopt that node's reported state in one step;
// `st` is the ENGAGE ack. Binding without adopting is not offered: the two were
// always done together, and a bind whose position had not yet been reconciled
// is a state no caller should be able to name.
void slotBind(uint8_t s, uint8_t n, const NodeStatus* st);

// Unbind slot `s`. A slot that holds no node holds no position either, so this
// also zeroes machinePos[s] and clears its homed and enabled bits.
void slotUnbind(uint8_t s);

// ─── Position datum, in the NODE frame ────────────────────────────────────────

// Record node `n`'s own counter as its origin: machinePos becomes a derived
// offset from here on and survives any later rebinding. Caller must have armed
// the node's continuity witness in the SAME transaction that produced `pos` --
// a separate read could straddle a reset and pair a witness with a stale count.
void originRecord(uint8_t n, int32_t pos);

// Destroy every position reference for node `n`: its origin, its parked-counter
// entry, and the homed bit of whatever slot it occupies. Both frames die
// together, here, or the two disagree.
//
// NAMING: this is the ORIGIN -- Core 0's stored reference. It is NOT the
// node-side datum (NODE_FLAG_DATUM / CMD_DATUM_SET), which is the node's own
// continuity witness; Core 0 never writes that. Two different facts.
void originInvalidate(uint8_t node);

// Whole-machine version -- estop, soft limit, disable-all.
void originInvalidateAll(void);

// True once an origin has been recorded for node `n` and nothing since has
// invalidated it. Core 0's half of the validity conjunction.
bool originValid(uint8_t n);

// ─── Frozen-while-parked check ────────────────────────────────────────────────
// A parked node can neither move nor count (its RX ISR returns on slot ==
// SLOT_NONE), so when it is engaged again its counter must read exactly what it
// read when parked. Any difference means a reboot or lost steps. Free: it rides
// acks we already pay for.

// Remember node `n`'s counter at the instant it was parked.
void parkRecord(uint8_t n, int32_t pos);

// Forget it -- the node did not answer, so we do not know where it stopped.
// Dropping the entry is required: a stale one produces a false match later.
void parkForget(uint8_t n);

// True if we hold a parked counter for `n` and it disagrees with `pos`.
// False when no entry is held -- an unknown park cannot accuse.
bool parkMoved(uint8_t n, int32_t pos);

// ─── Validity reconciliation ──────────────────────────────────────────────────
// Core 1 only ever SIGNALS a fault, by entering ALARM with a reason; this folds
// the signal into masks that Core 0 alone writes. Level-triggered, so it is
// idempotent and cannot miss a transition. Call from Core 0's loop before
// anything the host can observe.
void reconcileValidity(void);
