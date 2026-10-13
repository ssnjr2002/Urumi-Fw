#pragma once
#include <stdint.h>

// traverse.h — STATE_TRAVERSE: node legs that keep the origin (park, span,
// unlatch), outside a homing session (docs/homing.md §2.2).
//
// The first leg opens the state and every leg armed while the step runs joins
// it. The state closes itself when the step ends, through machineSettle: IDLE,
// or ALARM with F_LATCHED when a span ended on its switch. A failed park or
// unlatch fails the step as a homing leg does (F_HOMING); a span that
// found no switch is not a failure.

// Arm a span or unlatch leg (LegIntent) on `node`, as homingBegin. Returns
// nullptr, or the refusal text with nothing moved. Prints nothing.
const char* traverseBegin(uint8_t intent, uint8_t node, uint8_t dir,
                          uint16_t startUs, uint16_t floorUs,
                          uint16_t rampSteps, uint32_t maxSteps,
                          bool* intentMismatch = nullptr);

// Arm a park leg, as homingParkBegin.
const char* traverseParkBegin(uint8_t node, int32_t target, uint16_t startUs,
                              uint16_t floorUs, uint16_t rampSteps);

// Turn the runner's TRAVERSE events into the state. Call from the Core 0 loop,
// after runnerTick.
void traverseTick(void);
