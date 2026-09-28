#pragma once
#include <stdint.h>

// bus.h — the boot sweep and the two masks it leaves: which nodes are mute and
// which of those the operator has excluded (bit n = bus id n).
//
// Touched is derived, not stored: the node is in nodeEnabled or holds a slot,
// bound or fenced. Mute: no confirmed make-safe to the sweep, and named by the
// config or touched. An id nobody expects is an empty address.

// Make safe every bus id, rebuild `mute` from the answers and clear `excluded`.
// The boot sequence only, after Core 1 is released.
void busSweep(void);

// True while a mute node is not excluded: ALARM_BUS_DEGRADED.
bool busDegraded(void);

uint16_t busMute(void);
uint16_t busExcluded(void);
uint16_t busTouched(void);

// Exclude mute nodes. Returns false, changing nothing, if a bit in `ids` is not
// mute. Commands to an excluded node answer `excluded`, make-safe exempt.
bool busExclude(uint16_t ids);
