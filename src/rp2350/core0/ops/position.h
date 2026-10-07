#pragma once
#include <stdint.h>
#include "../../ipc/core1_rpc.h"    // NodeStatus

// position.h — the slot binding, the axes request and the node-frame position
// datum.
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
// nodeOrigin, so the next map cheerfully resurrected a dead datum. There is
// no way to express that mistake through this header.
//
// Core-0-private by design. Core 1 never sees the map; when the probe supervisor
// lands (docs/bus_alarm.md §4.4) it is handed an expected byte mask at arm time
// rather than the map itself.

#define SLOT_NONE     0xFF
#define MOTION_SLOTS  4     // stream-byte motion slots (X/Y/Z/A)
#define SLOT_X        0
#define SLOT_Y        1
#define SLOT_Z        2
#define SLOT_A        3

// ─── Slot binding ─────────────────────────────────────────────────────────────
// Which node listens on each stream slot, of any type (slot_map).

// Forget the axes request. The slot table itself (bindings and fences) is kept:
// it records what the nodes may still be doing, so only power-on clears it.
void slotMapReset(void);

// The bus id bound to slot `s`, or SLOT_NONE. Slot indices are 0..MOTION_SLOTS-1.
uint8_t slotNodeAt(uint8_t s);

// The slot holding bus id `n`, or SLOT_NONE.
uint8_t nodeSlot(uint8_t n);

// Bind slot `s` to node `n` and adopt that node's reported state in one step;
// `st` is the ENGAGE ack. Binding without adopting is not offered: the two were
// always done together, and a bind whose position had not yet been reconciled
// is a state no caller should be able to name.
void slotBind(uint8_t s, uint8_t n, const NodeStatus* st);

// Unbind slot `s`, clearing any fence: the caller has its node's confirmation.
// A slot that holds no node holds no position either, so this also zeroes
// machinePos[s] and clears its homed and enabled bits.
void slotUnbind(uint8_t s);

// Fence slot `s` on node `n`, which did not confirm leaving it. Nothing is known
// about `n` any more: its origin is invalidated and the slot's views cleared. A
// fenced slot keeps `n`, takes no engage, and unbinds its axis. Only a
// confirmed make-safe (or an engage elsewhere) from `n` frees it: slotUnbind.
void slotFence(uint8_t s, uint8_t n);
bool slotFencedAt(uint8_t s);

// ─── Axes request ─────────────────────────────────────────────────────────────
// The node each axis should be (axes_map), and which of them are pending: named
// but not yet confirmed a stepper. Axis k is BOUND while slot k holds its
// requested node and it is not pending. machinePos, axes_homed, homingLatched
// and axes_enabled are derived for bound axes only; an unbound axis reads 0,
// unhomed, unlatched, disabled. No request means no axis is bound.

// Store the request: four bus ids or SLOT_NONE, and a pending mask (bit k).
void axesReqSet(const uint8_t* ids, uint8_t pending);
uint8_t axesReqAt(uint8_t k);
uint8_t axesReqPending(void);
void axesReqClearPending(uint8_t k);
// Drop node `n` from the request: its axes become `-`, not pending.
void axesReqDrop(uint8_t n);
void axesReqForget(void);

// The node bound as axis k, or SLOT_NONE (also while slot k is fenced).
uint8_t axisNode(uint8_t k);

// The axis node `n` is bound as, or SLOT_NONE.
uint8_t nodeAxis(uint8_t n);
static inline bool node_isAxis(uint8_t n) { return nodeAxis(n) != SLOT_NONE; }

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

// Per-AXIS view of the above (bit0=X .. bit3=A), bound axes only.
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

// Datum each of `nodes[0..n)` at `steps[i]`: CMD_DATUM_SET, then originRecord
// from its ack. Every node is tried; one that fails has its origin
// invalidated. Returns nullptr, or the first failure's reason ("not_stepper",
// "no_datum" or an RPC result) with its node in `*badNode`. Does bus I/O.
const char* originDatum(const uint8_t* nodes, const int32_t* steps, uint8_t n,
                        uint8_t* badNode);

// Destroy every position reference for node `n`: its origin, its parked-counter
// entry, and the homed bit of whatever slot it occupies. Both frames die
// together, here, or the two disagree.
//
// NAMING: this is the ORIGIN -- Core 0's stored reference. It is NOT the
// node-side datum (NODE_FLAG_DATUM / CMD_DATUM_SET), which is the node's own
// continuity witness; Core 0 never writes that. Two different facts.
void originInvalidate(uint8_t node);

// Node `n` moved under its own pulser, with its witness intact (a finished park
// leg), and its counter now reads `nodePos`. Keep the origin and re-derive
// machinePos for its slot, as a bind does. No-op unless `n` is homed.
void originAdopt(uint8_t n, int32_t nodePos);

// The node counter at which homed node `n` stands at machine position
// `machineSteps`: the target of a park leg. Meaningful only if originValid(n).
int32_t originTarget(uint8_t n, int32_t machineSteps);

// Whole-machine version -- estop, soft limit, disable-all.
void originInvalidateAll(void);

// True once an origin has been recorded for node `n` and nothing since has
// invalidated it. Core 0's half of the validity conjunction.
bool originValid(uint8_t n);

// The nodes holding an origin, bit n = node n (get nodehomed=).
uint16_t originMask(void);

// ─── Tool probe, in the NODE frame ────────────────────────────────────────────
// The machine-frame Z at which node n's tool opened the bed switch. Keyed by bus
// id like the origin, so a parked head keeps its probe across axes_map swaps.
// Only valid while the origin it was measured against is: originRecord and both
// originInvalidate variants clear it.

// Store node `n`'s contact height. Ignored unless `n` is homed.
void probeRecord(uint8_t n, int32_t zSteps);

// Clear node `n`'s probe. Idempotent.
void probeForget(uint8_t n);

// True if node `n` holds a valid probe; writes it to `zSteps` when non-null.
bool probeValid(uint8_t n, int32_t* zSteps);

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
