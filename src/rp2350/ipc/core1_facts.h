// core1_facts.h — facts Core 1 reports to Core 0.
//
// A single-producer, single-consumer ring: Core 1 pushes, Core 0's observer
// pops. A fact reports something Core 1 has finished; it never asks Core 0 to
// act, and any state Core 1 writes is already published when its fact is read.

#pragma once
#include <stdint.h>

enum Core1Fact : uint8_t {
    FACT_NONE = 0,
    FACT_ESTOP_SWEPT,    // the estop sweep is done and ALARM published
    FACT_RING_DRAINED,   // planner motion at rest, ring empty, plannerActive clear
};

// Core 1. False when the ring is full: the fact is dropped and core1FactsLost
// is set.
bool core1FactPush(Core1Fact f);

// Core 0. FACT_NONE when empty.
Core1Fact core1FactPop(void);

// Set by Core 1 when a push found the ring full; read and cleared by Core 0.
bool core1FactsLost(void);
void core1FactsLostClear(void);

// Core 0, with Core 1 parked (soft reset).
void core1FactsReset(void);
