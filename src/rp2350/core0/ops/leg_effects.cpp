#include "leg_effects.h"
#include "position.h"
#include "../../ipc/shared_state.h"

// Both exits invalidate the origin, and that is not conservatism -- it is
// required. A leg moves the axis with the NODE's own pulser, so the node counts
// those steps and Core 1 does not; the machinePos it maintains is stale the
// moment the pulser runs. (`step` differs: it goes through the stream, so Core 1
// adds the same steps and both frames stay consistent -- see cmdStep.) Dropping
// the datum makes that staleness visible as an un-homed axis instead of a
// plausible wrong number, and §3.4's closing `setorigin` is what re-derives it
// from the node's counter.
//
// The one exception is a park that finished: it moved within the datum, with
// the witness intact and the counter on target, so the origin stands and only
// machinePos is re-derived. legAbortAll ends an acked park the same way.
void legEffectRelease(uint8_t intent, const LegEnd& e) {
    if (intent == LEG_DUMMY) return;
    if (intent == LEG_PARK && e.failWhy == LEGFAIL_NONE) originAdopt(e.node, e.pos);
    else                                                 originInvalidate(e.node);
}

// A failed leg leaves the latch untouched, and that is already right: a seek
// that failed never reached the switch, so its bit should stay clear, and a
// retract that failed never escaped one, so its bit should stay set.
//
// A succeeded leg's outcome is known without re-reading the pin: a seek ended
// ON the switch and a retract ended OFF it. Recorded against the NODE, which is
// what the switch is wired to; position.cpp keeps the slot-framed
// homingLatched in step and re-derives it across a rebind. Never for a sweep (a
// rotary axis has no switch to stand on) nor a park (it finished off its
// switch).
void legEffectEnd(uint8_t intent, const LegEnd& e) {
    if (e.failWhy == LEGFAIL_NONE && (intent == LEG_SEEK || intent == LEG_RETRACT))
        nodeLatchSet(e.node, !e.retract);
    legEffectRelease(intent, e);
}

bool legOwnerHolds(uint8_t owner) { return machineState == owner; }
