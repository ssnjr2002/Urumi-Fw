#pragma once
#include <stdint.h>
#include "../../config/config_decode.h"   // CfgAxis

// steps.h — the leg numbers the sequences share (web/src/homing/derive.ts) and
// the step builders (docs/plans/machine-model.md, Levels).

// A pulse interval for `feed` units/s, µs, clamped to the wire's u16.
uint16_t intervalUs(float feed, float spu);

// `units` in steps, at least 1.
uint32_t distSteps(double units, float spu);

// A machine coordinate in wire steps: + is physical, so a wiring fix flips it.
int32_t coordSteps(const CfgAxis& a, double units);

// Where a linear home leaves the axis (configParkPos), in steps.
int32_t parkSteps(const CfgAxis& a);

// Every present head's Z node, homed or not; returns how many.
uint8_t headZNodes(uint8_t* out);

// LEGS: park every present head's Z at its park position, in parallel, at its
// homing feeds. Each must be homed and enabled (the caller checks). Returns
// nullptr, or the refusal text; a refusal after a leg was armed has failed the
// sequence (homingAbandon).
const char* parkZs(void);

// RING: a straight XY line to machine mm (x, y) at `feed` mm/s, owned by the
// live sequence (RING_SEQ); the step ends when the ring drains. Returns
// nullptr, or the refusal text; `*queued` is false when it queued nothing
// (refused, or the head is there already).
const char* lineTo(float x, float y, float feed, bool* queued);
