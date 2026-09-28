// position.cpp — see position.h for what this module is and why it is one file.
//
// Lifted out of control_plane.cpp, where it sat as ~140 lines of statics that
// every command handler reached into directly. The logic is unchanged; what is
// new is that the arrays are now private and the callers go through named
// operations.

#include "position.h"
#include "../../ipc/shared_state.h"
#include "../usb_protocol.h"   // NODE_FLAG_*, BUS_ADDR_MAX
#include "hardware/sync.h"     // __dmb

// ─── Slot binding (docs/engage_and_axis_map.md §5) ────────────────────────────
// slotNode[i] = the bus id currently ENGAGE-bound to stream slot i, or
// SLOT_NONE. Core 0 owns this map and the abstraction; Core 1 only ever sees
// granular per-node CMD_ENGAGE.
static uint8_t slotNode[MOTION_SLOTS] = { SLOT_NONE, SLOT_NONE, SLOT_NONE, SLOT_NONE };
// bit s = slot s is fenced: slotNode[s] did not confirm leaving it (position.h).
static uint8_t slotFenced = 0;

// The axes request (position.h): axesReq[k] = the node axis k should be,
// axesPending bit k = named but not yet confirmed a stepper.
static uint8_t axesReq[MOTION_SLOTS] = { SLOT_NONE, SLOT_NONE, SLOT_NONE, SLOT_NONE };
static uint8_t axesPending = 0;

// ─── Position datum, in the NODE frame (docs/node_session_and_datum.md §2) ────
// machinePos[] is indexed by SLOT, so it goes stale the moment a slot is rebound to
// a different node. The datum therefore lives with the NODE instead:
// nodeOrigin[id] is that node's own step counter at the instant it was datumed,
// and machinePos[slot] = <node counter now> - nodeOrigin[node]. A parked node
// can neither move nor count, so the offset stays valid across an arbitrary
// number of swaps.
//
// axes_homed (per SLOT) is DERIVED from nodeHomed (per BUS ID) every time a slot
// is bound.
static int32_t  nodeOrigin[BUS_ADDR_MAX + 1] = {0};
static uint16_t nodeHomed = 0;               // bit n = nodeOrigin[n] is valid

// bit n = node n is standing on its limit switch. Truth; homingLatched is the
// slot-framed view, rebuilt on every bind (position.h).
static uint16_t nodeLatched = 0;
uint8_t homingLatched = 0;

void nodeLatchSet(uint8_t n, bool latched) {
    if (n > BUS_ADDR_MAX) return;
    if (latched) nodeLatched |=  (1u << n);
    else         nodeLatched &= ~(1u << n);
    const uint8_t s = nodeAxis(n);
    if (s != SLOT_NONE) {
        if (latched) homingLatched |=  (1 << s);
        else         homingLatched &= ~(1 << s);
    }
}

// parkPos[n] is node n's counter as reported by the ack of the CMD_ENGAGE that
// DISENGAGED it; parkSeen marks which entries are live.
//
// These MUST outlive one slot-map apply: a park lasts until some later
// command re-engages the node, which is the entire point. As locals they only
// ever checked nodes that stayed bound across a single command -- i.e. the ones
// that were never really parked. Every apply disengages all bound nodes
// before engaging any, so an entry is always refreshed before it is used.
static int32_t  parkPos[BUS_ADDR_MAX + 1] = {0};
static uint16_t parkSeen = 0;

// probeZ[n] is the machine-frame Z at which node n's tool opened the bed switch
// (docs/tool_probe_planner_integration.md). Machine frame means it is measured
// against nodeOrigin[n], so every path that changes or drops that origin clears
// the probe too: nodeProbed is always a subset of nodeHomed.
static int32_t  probeZ[BUS_ADDR_MAX + 1] = {0};
static uint16_t nodeProbed = 0;              // bit n = probeZ[n] is valid

// ─── Slot binding and axes request ────────────────────────────────────────────

void slotMapReset(void) {
    axesReqForget();
}

uint8_t slotNodeAt(uint8_t s) {
    return (s < MOTION_SLOTS) ? slotNode[s] : SLOT_NONE;
}

bool slotFencedAt(uint8_t s) {
    return s < MOTION_SLOTS && (slotFenced & (1 << s));
}

uint8_t nodeSlot(uint8_t n) {
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) if (slotNode[i] == n) return i;
    return SLOT_NONE;
}

uint8_t axisNode(uint8_t k) {
    if (k >= MOTION_SLOTS || ((axesPending | slotFenced) & (1 << k))) return SLOT_NONE;
    const uint8_t n = slotNode[k];
    return (n != SLOT_NONE && n == axesReq[k]) ? n : SLOT_NONE;
}

uint8_t nodeAxis(uint8_t n) {
    if (n == SLOT_NONE) return SLOT_NONE;
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) if (axisNode(k) == n) return k;
    return SLOT_NONE;
}

// An axis that stopped being bound without a rebind drops its views. One that
// became bound this way reads unbound-clean until its next bind adopts it; the
// callers always rebind right after.
static void axisViewsDrop(void) {
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) {
        if (axisNode(k) != SLOT_NONE) continue;
        machinePos[k]  = 0;
        axes_homed    &= ~(1 << k);
        homingLatched &= ~(1 << k);
    }
}

void axesReqSet(const uint8_t* ids, uint8_t pending) {
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) axesReq[k] = ids[k];
    axesPending = pending & 0x0F;
    axisViewsDrop();
}

uint8_t axesReqAt(uint8_t k) {
    return (k < MOTION_SLOTS) ? axesReq[k] : SLOT_NONE;
}

uint8_t axesReqPending(void) { return axesPending; }

void axesReqClearPending(uint8_t k) {
    if (k < MOTION_SLOTS) axesPending &= ~(1 << k);
}

void axesReqDrop(uint8_t n) {
    for (uint8_t k = 0; k < MOTION_SLOTS; k++)
        if (axesReq[k] == n) { axesReq[k] = SLOT_NONE; axesPending &= ~(1 << k); }
    axisViewsDrop();
}

void axesReqForget(void) {
    for (uint8_t k = 0; k < MOTION_SLOTS; k++) axesReq[k] = SLOT_NONE;
    axesPending = 0;
    axisViewsDrop();
}

// ─── The one place a position reference dies ──────────────────────────────────

void originInvalidate(uint8_t node) {
    if (node > BUS_ADDR_MAX) return;
    nodeHomed  &= ~(1u << node);
    parkSeen   &= ~(1u << node);       // its parked counter means nothing now
    nodeProbed &= ~(1u << node);
    uint8_t s = nodeAxis(node);
    if (s != SLOT_NONE) axes_homed &= ~(1 << s);
}

void originInvalidateAll(void) {
    nodeHomed  = 0;
    parkSeen   = 0;
    nodeProbed = 0;
    axes_homed = 0;
}

void originRecord(uint8_t n, int32_t nodePos, int32_t machineSteps) {
    if (n > BUS_ADDR_MAX) return;
    // The stored origin is the offset, not the counter: it is what
    // slotAdoptStatus subtracts on every rebind. Writing `nodePos` here and
    // `machineSteps` into machinePos separately would put the datum in two
    // places that a later bind could disagree about.
    nodeOrigin[n] = nodePos - machineSteps;
    nodeHomed    |= (1u << n);
    nodeProbed   &= ~(1u << n);        // measured against the origin just replaced
    uint8_t s = nodeAxis(n);
    if (s != SLOT_NONE) { machinePos[s] = machineSteps; axes_homed |= (1 << s); }
}

bool originValid(uint8_t n) {
    return n <= BUS_ADDR_MAX && (nodeHomed & (1u << n)) != 0;
}

// ─── Tool probe ───────────────────────────────────────────────────────────────

void probeRecord(uint8_t n, int32_t zSteps) {
    if (n > BUS_ADDR_MAX || !(nodeHomed & (1u << n))) return;
    probeZ[n]   = zSteps;
    nodeProbed |= (1u << n);
}

void probeForget(uint8_t n) {
    if (n <= BUS_ADDR_MAX) nodeProbed &= ~(1u << n);
}

bool probeValid(uint8_t n, int32_t* zSteps) {
    if (n > BUS_ADDR_MAX || !(nodeProbed & (1u << n))) return false;
    if (zSteps) *zSteps = probeZ[n];
    return true;
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

    // NODE_FLAG_ENABLED is deliberately NOT read here any more. Core 1 already
    // folded this same ack's flags byte into nodeEnabled (core1/rpc_server.cpp),
    // and reconcileValidity projects that onto the slot -- so the fact arrives
    // by the same route whether or not a bind happened. Adopting it here as well
    // was the only thing keeping energisation alive across a rebind, and it
    // failed silently when the ack was missing (`flags` falls back to 0, which
    // reads as "disabled" for a node nobody asked about).

    // The node's continuity witness is broken (reset, or de-energised at some
    // point) -- whatever origin we hold for it no longer refers to anything.
    if (!(flags & NODE_FLAG_DATUM)) originInvalidate(n);

    // A node in a slot that is not its axis (a probe's vacuum, a bench
    // slot_map) has no axis views.
    if (axisNode(s) != n) {
        machinePos[s]  = 0;
        axes_homed    &= ~(1 << s);
        homingLatched &= ~(1 << s);
        return;
    }

    // Adopted, not assumed. The node has been sitting on (or off) its switch
    // the whole time it was parked, so the fact travels with the node and the
    // incoming slot inherits it rather than the outgoing node's.
    if (nodeLatched & (1u << n)) homingLatched |=  (1 << s);
    else                         homingLatched &= ~(1 << s);

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

void slotFence(uint8_t s, uint8_t n) {
    if (s >= MOTION_SLOTS || n == SLOT_NONE) return;
    originInvalidate(n);               // while its axis bit still points here
    slotNode[s]    = n;
    slotFenced    |= (1 << s);
    machinePos[s]  = 0;
    axes_homed    &= ~(1 << s);
    homingLatched &= ~(1 << s);
}

void slotUnbind(uint8_t s) {
    if (s >= MOTION_SLOTS) return;
    slotNode[s]    = SLOT_NONE;
    slotFenced    &= ~(1 << s);
    machinePos[s]  = 0;
    axes_homed    &= ~(1 << s);
    // axes_enabled needs no clear: the projection in reconcileValidity reads
    // SLOT_NONE and contributes no bit. nodeEnabled is likewise untouched, for
    // the same reason nodeLatched is below -- unbinding a slot de-energises
    // nothing, and the node keeps holding torque while parked.
    // The NODE's latch (nodeLatched) is deliberately untouched -- unbinding a
    // slot does not move anything off a switch. Only the slot-framed view is
    // dropped, because an empty slot cannot be latched.
    homingLatched &= ~(1 << s);
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
// never have cleared -- without that, the next axes_map would happily resurrect
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

    // Edge-triggered, not level-triggered. The documented ESTOP recovery flow
    // (axes_enable on -> setorigin -> unalarm, cmd/axis.cpp's cmdAxesEnable
    // comment) runs entirely from INSIDE the same ALARM_ESTOP condition this
    // clear keys on. A level trigger re-ran it every loop pass for as long as
    // that condition held, which stomped the very recovery it exists to gate: a
    // setorigin taken mid-recovery was erased one tick later, before the host's
    // next command. It now fires once, on the rising edge, and re-arms only once
    // the condition has gone false again.
    static bool originLatched = false;

    // Position dies the instant motion stops abruptly -- before the bus sweep.
    bool originActive = (st == STATE_ESTOP || ar == ALARM_ESTOP || ar == ALARM_SOFT_LIMIT);
    if (originActive && !originLatched) {
        originInvalidateAll();
        originLatched = true;
    } else if (!originActive) {
        originLatched = false;
    }

    // An estop fences every bound slot: its make-safe replies stay on Core 1,
    // so no node here is confirmed released until `unstop` asks again.
    static bool fenceLatched = false;
    const bool fenceActive = (st == STATE_ESTOP || ar == ALARM_ESTOP);
    if (fenceActive && !fenceLatched) {
        for (uint8_t s = 0; s < MOTION_SLOTS; s++)
            if (slotNode[s] != SLOT_NONE) slotFence(s, slotNode[s]);
        fenceLatched = true;
    } else if (!fenceActive) {
        fenceLatched = false;
    }

    // Energisation is not folded in from a state signal at all any more -- it is
    // PROJECTED from node truth. Core 1 clears nodeEnabled inside the sweep
    // itself (bus/packet.cpp), and the sweep completes before STATE_ALARM is
    // published (core1/core1.cpp), so the documented invariant is unchanged and
    // now holds without this function having to infer anything: once ALARM is
    // observable, the mask that produced `enabled=` was already zeroed by the
    // sweep that parked the bus.
    //
    // That is also why this one is an unconditional recompute rather than an
    // edge-triggered clear, and why it cannot repeat the bug above: it destroys
    // no information, it restates node truth in slot terms. Whatever the node
    // frame says right now is what the slot frame says right now.
    uint8_t e = 0;
    for (uint8_t s = 0; s < MOTION_SLOTS; s++) {
        const uint8_t n = axisNode(s);
        if (n != SLOT_NONE && (nodeEnabled & (1u << n))) e |= (1 << s);
    }
    axes_enabled = e;
}
