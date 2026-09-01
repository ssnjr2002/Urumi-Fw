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
//   Cross-core       machinePos[] and nodeEnabled (Core 1 writes both),
//                    axes_homed, axes_enabled
//   Node-reported    NODE_FLAG_DATUM, the node's own step counter
//
// nodeEnabled is the one node-frame mask that is NOT a Core-0 static, because it
// is the one with an asynchronous writer -- Core 1 sweeps the bus safe on estop
// and soft reset without being asked. It is declared in ipc/shared_state.h with
// the reasoning; reconcileValidity() projects it onto axes_enabled here, the
// same way axes_homed and homingLatched are projections of nodeHomed and
// nodeLatched. The rule the three share: the NODE frame is the truth, the SLOT
// frame is a view, and a view is never written directly by a command handler.
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

// ─── Limit latch, in the NODE frame ───────────────────────────────────────────
//
// Same shape as the datum below, and for the same reason. A limit switch is
// wired to a NODE; whether it is held down is a fact about that node's
// mechanism and has nothing to do with which stream slot the node currently
// occupies. Stored per slot it went stale on the first rebind: homing Z on
// head 0 (slot 2 = node 3) and then binding slot 2 to node 5 left the slot bit
// asserting that head 1's Z was on a switch it had never touched -- and since
// the mask gates ALARM_LIMIT_LATCHED, that held the machine in an alarm no
// `unalarm` could clear.
//
// So `homingLatched` (per SLOT) is DERIVED from this on every bind, exactly as
// axes_homed is derived from nodeHomed. The derived mask then means the useful
// thing: latched switches among the axes the machine is CURRENTLY driving. An
// unbound node's latch stops gating the machine and comes back when that node
// is bound again -- which is right, because the node really will still refuse
// stream steps.
void nodeLatchSet(uint8_t n, bool latched);

// Per-SLOT view of the above (bit0=X .. bit3=A), rebuilt by slotBind/slotUnbind.
// Read-only to everything but position.cpp.
extern uint8_t homingLatched;

// ─── Position datum, in the NODE frame ────────────────────────────────────────

// Record that node `n`'s own counter `nodePos` corresponds to machine position
// `machineSteps`. machinePos becomes a derived offset from here on and survives
// any later rebinding. Caller must have armed the node's continuity witness in
// the SAME transaction that produced `nodePos` -- a separate read could straddle
// a reset and pair a witness with a stale count.
//
// What is stored is the DIFFERENCE, so slotAdoptStatus's
// `machinePos[s] = st->pos - nodeOrigin[n]` keeps working untouched: the datum
// lives in exactly one form, and a non-zero one costs no second field.
//
// machineSteps is 0 for a switch at the origin end and hardTravel × stepsPerUnit
// for one at the far end (docs/homing.md §2.5). The host does the conversion --
// nodeOrigin and machinePos are both the WIRE frame, which is steps.
void originRecord(uint8_t n, int32_t nodePos, int32_t machineSteps);

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
