// position.cpp — see position.h for what this module is and why it is one file.
//
// Lifted out of control_plane.cpp, where it sat as ~140 lines of statics that
// every command handler reached into directly. The logic is unchanged; what is
// new is that the arrays are now private and the callers go through named
// operations.

#include "position.h"
#include "../ipc/shared_state.h"
#include "usb_protocol.h"      // NODE_FLAG_*, BUS_ADDR_MAX
#include "hardware/sync.h"     // __dmb

// ─── Axis map (docs/engage_and_axis_map.md §5) ────────────────────────────────
// slotNode[i] = the bus id currently ENGAGE-bound to stream slot i, or
// SLOT_NONE. Core 0 owns this map and the abstraction; Core 1 only ever sees
// granular per-node CMD_ENGAGE.
static uint8_t slotNode[MOTION_SLOTS] = { SLOT_NONE, SLOT_NONE, SLOT_NONE, SLOT_NONE };

// ─── Position datum, in the NODE frame (docs/node_session_and_datum.md §2) ────
// machinePos[] is indexed by SLOT, so it goes stale the moment axis_map rebinds
// a slot to a different node. The datum therefore lives with the NODE instead:
// nodeOrigin[id] is that node's own step counter at the instant it was datumed,
// and machinePos[slot] = <node counter now> - nodeOrigin[node]. A parked node
// can neither move nor count, so the offset stays valid across an arbitrary
// number of swaps.
//
// axes_homed (per SLOT) is DERIVED from nodeHomed (per BUS ID) every time a slot
// is bound.
static int32_t  nodeOrigin[BUS_ADDR_MAX + 1] = {0};
static uint16_t nodeHomed = 0;               // bit n = nodeOrigin[n] is valid

// parkPos[n] is node n's counter as reported by the ack of the CMD_ENGAGE that
// DISENGAGED it; parkSeen marks which entries are live.
//
// These MUST outlive one axis_map invocation: a park lasts until some later
// command re-engages the node, which is the entire point. As locals they only
// ever checked nodes that stayed bound across a single command -- i.e. the ones
// that were never really parked. Every axis_map disengages all bound nodes
// before engaging any, so an entry is always refreshed before it is used.
static int32_t  parkPos[BUS_ADDR_MAX + 1] = {0};
static uint16_t parkSeen = 0;

// ─── Axis map ─────────────────────────────────────────────────────────────────

void axisMapReset(void) {
    for (int i = 0; i < MOTION_SLOTS; i++) slotNode[i] = SLOT_NONE;
}

uint8_t slotNodeAt(uint8_t s) {
    return (s < MOTION_SLOTS) ? slotNode[s] : SLOT_NONE;
}

uint8_t nodeSlot(uint8_t n) {
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) if (slotNode[i] == n) return i;
    return SLOT_NONE;
}

// ─── The one place a position reference dies ──────────────────────────────────

void originInvalidate(uint8_t node) {
    if (node > BUS_ADDR_MAX) return;
    nodeHomed &= ~(1u << node);
    parkSeen  &= ~(1u << node);        // its parked counter means nothing now
    uint8_t s = nodeSlot(node);
    if (s != SLOT_NONE) axes_homed &= ~(1 << s);
}

void originInvalidateAll(void) {
    nodeHomed  = 0;
    parkSeen   = 0;
    axes_homed = 0;
}

void originRecord(uint8_t n, int32_t pos) {
    if (n > BUS_ADDR_MAX) return;
    nodeOrigin[n] = pos;
    nodeHomed    |= (1u << n);
    uint8_t s = nodeSlot(n);
    if (s != SLOT_NONE) { machinePos[s] = 0; axes_homed |= (1 << s); }
}

bool originValid(uint8_t n) {
    return n <= BUS_ADDR_MAX && (nodeHomed & (1u << n)) != 0;
}

// ─── Frozen-while-parked check ────────────────────────────────────────────────

void parkRecord(uint8_t n, int32_t pos) {
    if (n > BUS_ADDR_MAX) return;
    parkPos[n] = pos;
    parkSeen  |= (1u << n);
}

void parkForget(uint8_t n) {
    if (n <= BUS_ADDR_MAX) parkSeen &= ~(1u << n);
}

bool parkMoved(uint8_t n, int32_t pos) {
    return n <= BUS_ADDR_MAX && (parkSeen & (1u << n)) && parkPos[n] != pos;
}

// ─── Binding, and the three-domain join ───────────────────────────────────────

// Rebuild slot `s`'s position and enabled bit from the status payload the node
// returned with its ENGAGE ack -- no second transaction, and no window in which
// the node could have rebooted between binding and reporting.
//
// `st` is the ack payload, or nullptr if the node did not answer. Both flags are
// taken from the NODE's own report rather than from what Core 0 last assumed it
// commanded -- that is the point: the slot view becomes derived from node truth
// at every bind.
//
// Validity is a CONJUNCTION of two things neither side can know alone:
//   nodeHomed[n]     -- Core 0: "I took a datum for this node"
//   NODE_FLAG_DATUM  -- the node: "nothing since has interrupted it"
// The node's half covers events Core 0 never observes (brownout, watchdog reset,
// a de-energise it did not issue). Core 0's half covers a node that has simply
// never been datumed in this machine's frame.
static void slotAdoptStatus(uint8_t s, uint8_t n, const NodeStatus* st) {
    const bool    haveTail = (st != nullptr && st->hasStepperTail);
    const uint8_t flags    = (st != nullptr) ? st->flags : 0;

    if (flags & NODE_FLAG_ENABLED) axes_enabled |=  (1 << s);
    else                           axes_enabled &= ~(1 << s);

    // The node's continuity witness is broken (reset, or de-energised at some
    // point) -- whatever origin we hold for it no longer refers to anything.
    if (!(flags & NODE_FLAG_DATUM)) originInvalidate(n);

    if (haveTail && (nodeHomed & (1u << n))) {
        machinePos[s] = st->pos - nodeOrigin[n];
        axes_homed   |= (1 << s);
    } else {
        machinePos[s] = 0;
        axes_homed   &= ~(1 << s);
    }
}

void slotBind(uint8_t s, uint8_t n, const NodeStatus* st) {
    if (s >= MOTION_SLOTS) return;
    slotNode[s] = n;
    slotAdoptStatus(s, n, st);
}

void slotUnbind(uint8_t s) {
    if (s >= MOTION_SLOTS) return;
    slotNode[s]   = SLOT_NONE;
    machinePos[s] = 0;
    axes_homed   &= ~(1 << s);
    axes_enabled &= ~(1 << s);
}

// ─── Validity reconciliation — Core 0 is the sole writer ──────────────────────
// axes_homed / axes_enabled are bitmasks Core 0 read-modify-writes (|= and &=).
// Core 1 used to whole-byte-write them on estop and soft limit, which raced
// those RMWs: Core 0 reading a mask, Core 1 zeroing it, Core 0 writing back its
// stale value -- an axis left claiming a datum the estop had just destroyed.
// There is no atomic here and no critical section; instead Core 1 only ever
// SIGNALS, by entering ALARM with a reason, and this folds the signal in.
//
// It also reaches nodeOrigin/nodeHomed, which are Core-0 statics Core 1 could
// never have cleared -- without that, the next axis_map would happily resurrect
// a datum an estop had destroyed.
//
// Both the text plane and STATUS_RSP are answered from Core 0's loop, so the
// documented invariant holds: once ALARM is visible, the datum is already gone
// and the bus is already parked.
void reconcileValidity(void) {
    // Read state BEFORE reason, with a barrier between: Core 1 publishes them in
    // the opposite order (reason, __dmb, state) so that observing the new state
    // implies the reason is already there. Loading both without a barrier let the
    // compiler or the core reorder these two loads, which reads the new state
    // against the STALE reason -- e.g. STATE_ALARM paired with ALARM_NONE, which
    // matches none of the tests below and silently skips the invalidation.
    uint8_t st = machineState;
    __dmb();
    uint8_t ar = alarmReason;

    // Position dies the instant motion stops abruptly -- before the bus sweep.
    if (st == STATE_ESTOP || ar == ALARM_ESTOP || ar == ALARM_SOFT_LIMIT)
        originInvalidateAll();
    // Energisation, however, is only false once Core 1's busDisableAll() has
    // actually run. Core 1 sets ALARM_ESTOP *before* the sweep and STATE_ALARM
    // *after* it, so the conjunction is precisely "the sweep has completed".
    // Keying on STATE_ESTOP instead would report the machine disarmed while
    // every EN pin was still asserted.
    if (st == STATE_ALARM && ar == ALARM_ESTOP) axes_enabled = 0;
}
