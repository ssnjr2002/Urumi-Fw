#pragma once
#include <stdint.h>

// The `goto` sequences (docs/plans/position-moves.md). Published as JOGGING
// with reason GOTO throughout; a failed park is F_HOMING; `stop` ends it as any
// sequence. Each returns nullptr once started (or with nothing to move), or the
// refusal text after `err ` with nothing moved. Gating the machine state is the
// caller's.

// Park every present head's Z (LEGS), then the anchor in a straight line to
// machine mm (mx, my) at `feed` (RING, owner RING_SEQ). X, Y and every present
// Z must be homed. `aUnlock`: the selected A rides the line to unlock (machine
// 0), as steps.h lineTo.
const char* gotoLine(float mx, float my, float feed, bool aUnlock);

// Slot k (Z or A, the selected head's) to machine `to` at `feed`, through the
// planner (RING). That axis must be homed; no Z park.
const char* gotoAxis(uint8_t k, float to, float feed);

// LEGS: every present head's Z to its park position (`up`), or every present
// head's A to machine 0 (`unlock`), at today's park speeds (steps.h parkZs,
// unlockAs). Each must be homed.
const char* gotoUp(void);
const char* gotoUnlock(void);

// LEGS: head h's Z to its park position (`z`), then its A to machine `deg`
// (`a`), each skipped when the head has none or it is there already. Each
// moved must be homed. `unmount`, and `mount` locking A.
const char* gotoHead(uint8_t h, bool z, bool a, float deg);
