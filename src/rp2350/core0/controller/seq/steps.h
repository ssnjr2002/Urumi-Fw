#pragma once
#include <stdint.h>
#include "../../config/config_decode.h"   // CfgAxis
#include "../../planner/queue.h"          // PlannerQueueResult

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

// Every present head's Z homed, and answering an enable, before anything moves.
// Returns nullptr, or the refusal text (`not_homed`, `node N excluded|mute|…`).
const char* zsReady(void);

// LEGS: park every present head's Z at its park position, in parallel, at its
// homing feeds. Each must be homed and enabled (the caller checks). Returns
// nullptr, or the refusal text; a refusal after a leg was armed has failed the
// sequence (homingAbandon).
const char* parkZs(void);

// Every present head's A node; returns how many.
uint8_t headANodes(uint8_t* out);

// zsReady for every present head's A (`a_not_homed`).
const char* asReady(void);

// zsReady for head h's Z (`not_homed`) or A (`a_not_homed`); nullptr when
// the head has none.
const char* zReady(uint8_t h);
const char* aReady(uint8_t h);

// LEGS: every present head's A to machine 0, unlocking its holder, in
// parallel. The target is absolute, so a cabled A unwinds whatever it turned.
// As parkZs otherwise.
const char* unlockAs(void);

// LEGS: head h's Z to its park position, or its A to machine `deg`, as parkZs
// and unlockAs for one head. The axis must be present and ready.
const char* parkZ(uint8_t h);
const char* turnA(uint8_t h, float deg);

// Head h's Z at its park position, or its A at machine `deg` (within a step).
bool zParked(uint8_t h);
bool aAt(uint8_t h, float deg);

// Head h's A, machine degrees; false if it has none or it is unhomed.
bool aDeg(uint8_t h, float* deg);

// A planner refusal as its reply word (`full`, `soft_limit`, …).
const char* queueText(PlannerQueueResult r);

// A line that unlocks (lineTo's aUnlock) is possible: the selected head has no
// A, or A is homed. nullptr, or the refusal text.
const char* aUnlockReady(void);

// RING: a straight XY line to machine mm (x, y) at `feed` mm/s, owned by the
// live sequence (RING_SEQ); the step ends when the ring drains. `aUnlock`: A
// rides it to machine 0, unlocking the holder (no A on the head: XY only; A
// unhomed: a_not_homed). Returns nullptr, or the refusal text; `*queued` is
// false when it queued nothing (refused, or the head is there already).
const char* lineTo(float x, float y, float feed, bool* queued, bool aUnlock = false);
