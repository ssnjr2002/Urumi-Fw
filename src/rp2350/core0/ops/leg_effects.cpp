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
// The exception is a leg that moves within the datum -- a park, span or
// unlatch -- and ended with the witness intact: the origin stands and only
// machinePos is re-derived from the node's counter. legAbortAll ends an acked
// park the same way. A leg dropped without a verdict carries no witness.
static bool keepsOrigin(uint8_t intent) {
    return intent == LEG_PARK || intent == LEG_SPAN || intent == LEG_UNLATCH;
}

void legEffectRelease(uint8_t intent, const LegEnd& e) {
    if (intent == LEG_DUMMY) return;
    if (keepsOrigin(intent) && e.datum && !legEndFailed(intent, e))
        originAdopt(e.node, e.pos);
    else
        originInvalidate(e.node);
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
    const bool switched = intent == LEG_SEEK || intent == LEG_RETRACT ||
                          intent == LEG_SPAN || intent == LEG_UNLATCH;
    if (e.failWhy == LEGFAIL_NONE && switched) nodeLatchSet(e.node, !e.retract);
    legEffectRelease(intent, e);
}

bool legOwnerHolds(uint8_t owner) { return !estopRequested && machineContext == owner; }
