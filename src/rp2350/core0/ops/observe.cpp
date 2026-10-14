#include <Arduino.h>
#include "observe.h"
#include "position.h"
#include "machine.h"
#include "sequence.h"
#include "hardware/sync.h"
#include "../../ipc/core1_facts.h"
#include "../../ipc/shared_state.h"

// A drain is stale if the ring took more since; a new one follows that.
static void ringDrained(void) {
    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool empty = plannerRing.count() == 0 && !plannerActive;
    spin_unlock(plannerLock, s);
    if (empty) sequenceRingDrained();
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
            default: break;
        }
    }
    if (core1FactsLost()) factsLost();
    projectEnabled();
}
