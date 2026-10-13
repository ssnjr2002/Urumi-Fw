#pragma once
#include <stdint.h>

// traverse.h — node legs that keep the origin (park, span, unlatch), outside a
// homing context (docs/homing.md §2.2). Published as TRAVERSE.
//
// Each leg is a one-step `traverse` sequence, in the context it started in
// (IDLE). When it ends the machine publishes IDLE, or ALARM with
// F_LATCHED when a span ended on its switch. A failed park or
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
