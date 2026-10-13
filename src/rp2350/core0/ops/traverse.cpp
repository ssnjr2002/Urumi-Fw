#include <Arduino.h>
#include "traverse.h"
#include "machine.h"
#include "common.h"            // HOMING_KIND_LIMIT
#include "runner.h"
#include "leg_effects.h"
#include "homing.h"
#include "../../ipc/shared_state.h"
#include "hardware/sync.h"     // __dmb

// A leg step outside a context: published as TRAVERSE while it lives.
static void traverseOpen(void) { machinePublish(); }

const char* traverseBegin(uint8_t intent, uint8_t node, uint8_t dir,
                          uint16_t startUs, uint16_t floorUs,
                          uint16_t rampSteps, uint32_t maxSteps,
                          bool* intentMismatch) {
    const char* why = runnerArm(CTX_IDLE, intent, node, HOMING_KIND_LIMIT, dir,
                                intent == LEG_UNLATCH, startUs, floorUs, rampSteps,
                                maxSteps, intentMismatch);
    if (!why) traverseOpen();
    return why;
}

const char* traverseParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                              uint16_t floorUs, uint16_t rampSteps) {
    const char* why = runnerArmPark(CTX_IDLE, node, target, startUs, floorUs, rampSteps);
    if (!why) traverseOpen();
    return why;
}

// The runner has applied each end's effects (latch, origin). An aborted leg
// posts no end; its step still ends.
void traverseTick(void) {
    RunEvent ev;
    while (runnerPop(CTX_IDLE, &ev)) {
        if (ev.kind == RUN_LEG_END) {
            if (legEndFailed(ev.intent, ev.end)) homingAbandon(ev.end.node, ev.end.failWhy);
            continue;
        }
        machinePublish();
    }
}
