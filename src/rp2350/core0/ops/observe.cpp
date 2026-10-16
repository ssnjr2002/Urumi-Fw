#include <Arduino.h>
#include "observe.h"
#include "position.h"
#include "machine.h"
#include "sequence.h"
#include "../planner/queue.h"
#include "hardware/sync.h"
#include "../../ipc/core1_facts.h"
#include "../../ipc/shared_state.h"

// A drain is stale if the ring took more since; a new one follows that. A
// held job's lift is done: it lands PAUSED.
static void ringDrained(void) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool empty = plannerRing.count() == 0 && !plannerActive;
    spin_unlock(plannerLock, s);
    if (!empty) return;
    plannerLandPaused();
    sequenceRingDrained();
}

// A pause is at rest: lift and switch the tool off, then PAUSED on the drain;
// PAUSED now if there is nothing to do.
static void held(void) {
    if (!plannerQueueLift(JOB_HOLDING)) plannerLandPaused();
}

// A fact was dropped. Only an estop's sweep has an effect here: past it the
// fenced positions are cleared as the fact would have.
static void factsLost(void) {
    if (estopLatched() && estopSwept) slotFencedPosClear();
    faultSet(F_FACTS);
    core1FactsLostClear();
}

void observeTick(void) {
    for (Core1Fact f; (f = core1FactPop()) != FACT_NONE;) {
        switch (f) {
            case FACT_ESTOP_SWEPT:  slotFencedPosClear(); break;
            case FACT_RING_DRAINED: ringDrained(); break;
            case FACT_HELD:         held(); break;
            case FACT_TOOL_FAILED:  faultSet(F_TOOL); break;
            case FACT_DUTY_HELD:    plannerDutyLift(); break;
            default: break;
        }
    }
    if (core1FactsLost()) factsLost();
    projectEnabled();
}
