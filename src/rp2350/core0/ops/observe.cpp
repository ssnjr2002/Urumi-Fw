#include <Arduino.h>
#include "observe.h"
#include "position.h"
#include "machine.h"
#include "../../ipc/core1_facts.h"
#include "../../ipc/shared_state.h"

// A fact was dropped. Only an estop's sweep has an effect here: past it (the
// latch set, ALARM published) the fenced positions are cleared as the fact
// would have.
static void factsLost(void) {
    if (estopLatched() && machineState == STATE_ALARM) slotFencedPosClear();
    faultSet(F_FACTS);
    core1FactsLostClear();
}

void observeTick(void) {
    for (Core1Fact f; (f = core1FactPop()) != FACT_NONE;) {
        switch (f) {
            case FACT_ESTOP_SWEPT:  slotFencedPosClear(); break;
            case FACT_RING_DRAINED: break;   // the drain closes no session yet
            default: break;
        }
    }
    if (core1FactsLost()) factsLost();
    projectEnabled();
}
