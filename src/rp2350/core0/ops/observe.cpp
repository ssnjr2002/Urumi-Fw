#include <Arduino.h>
#include "observe.h"
#include "position.h"
#include "../../ipc/core1_facts.h"
#include "../../ipc/shared_state.h"
#include "hardware/sync.h"

// A fact was dropped. Only an estop's sweep has an effect here, and ALARM_ESTOP
// still says whether one happened. The alarm is raised from IDLE only, under
// the lock Core 1 enters motion under; elsewhere it waits, and an alarm already
// raised keeps its own reason.
static void factsLost(void) {
    const uint8_t st = machineState;
    __dmb();   // state before reason, the reverse of how they are published
    if (st == STATE_ALARM && alarmReason == ALARM_ESTOP) slotFencedPosClear();
    if (st == STATE_ALARM) { core1FactsLostClear(); return; }

    const uint32_t s = spin_lock_blocking(plannerLock);
    const bool idle = machineState == STATE_IDLE;
    if (idle) {
        alarmReason = ALARM_FACT_RING_FULL;
        __dmb();
        machineState = STATE_ALARM;
    }
    spin_unlock(plannerLock, s);
    if (idle) core1FactsLostClear();
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
