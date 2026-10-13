#include <Arduino.h>
#include "traverse.h"
#include "common.h"            // HOMING_KIND_LIMIT
#include "runner.h"
#include "sequence.h"
#include "leg_effects.h"

// The runner has applied the leg's effects (latch, origin); a failed leg was
// the driver's (homingFail).
static bool traverseNext(uint8_t) { return false; }

static const Sequence traverseSequence = { SEQ_TRAVERSE, traverseNext, nullptr };

static const char* started(const char* why) {
    if (!why) sequenceStart(&traverseSequence);
    return why;
}

const char* traverseBegin(uint8_t intent, uint8_t node, uint8_t dir,
                          uint16_t startUs, uint16_t floorUs,
                          uint16_t rampSteps, uint32_t maxSteps,
                          bool* intentMismatch) {
    return started(runnerArm(intent, node, HOMING_KIND_LIMIT, dir,
                             intent == LEG_UNLATCH, startUs, floorUs, rampSteps,
                             maxSteps, intentMismatch));
}

const char* traverseParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                              uint16_t floorUs, uint16_t rampSteps) {
    return started(runnerArmPark(node, target, startUs, floorUs, rampSteps));
}
