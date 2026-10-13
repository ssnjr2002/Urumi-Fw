#include <Arduino.h>
#include "traverse.h"
#include "machine.h"
#include "common.h"            // HOMING_KIND_LIMIT
#include "runner.h"
#include "leg_effects.h"
#include "homing.h"
#include "../../ipc/shared_state.h"
#include "hardware/sync.h"     // __dmb

// The first leg opens the state. An unlatch opens it with F_LATCHED set; the
// close re-derives the state from the mask.
static void traverseOpen(void) {
    if (machineState == STATE_TRAVERSE) return;
    machineState = STATE_TRAVERSE;
}

const char* traverseBegin(uint8_t intent, uint8_t node, uint8_t dir,
                          uint16_t startUs, uint16_t floorUs,
                          uint16_t rampSteps, uint32_t maxSteps,
                          bool* intentMismatch) {
    const char* why = runnerArm(STATE_TRAVERSE, intent, node, HOMING_KIND_LIMIT, dir,
                                intent == LEG_UNLATCH, startUs, floorUs, rampSteps,
                                maxSteps, intentMismatch);
    if (!why) traverseOpen();
    return why;
}

const char* traverseParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                              uint16_t floorUs, uint16_t rampSteps) {
    const char* why = runnerArmPark(STATE_TRAVERSE, node, target, startUs, floorUs, rampSteps);
    if (!why) traverseOpen();
    return why;
}

// The runner has applied each end's effects (latch, origin). An aborted leg
// posts no end; its step still ends and closes the state.
void traverseTick(void) {
    RunEvent ev;
    while (runnerPop(STATE_TRAVERSE, &ev)) {
        if (ev.kind == RUN_LEG_END) {
            if (legEndFailed(ev.intent, ev.end)) homingAbandon(ev.end.node, ev.end.failWhy);
            continue;
        }
        if (machineState == STATE_TRAVERSE) machineSettle();
    }
}
