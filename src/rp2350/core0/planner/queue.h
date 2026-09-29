#pragma once
#include <stdint.h>

// queue.h — Core 0's side of planner motion: lines into the shared ring.
//
// Core 1 runs what is queued (core1/emit/follower.cpp). Accepted in IDLE, and
// while planner motion runs; refused once a pause or abort is requested.

enum PlannerQueueResult : uint8_t {
    PQ_OK = 0,
    PQ_BAD_STATE,   // not IDLE or running planner motion, or stopping
    PQ_NO_CONFIG,   // no valid machine config
    PQ_NO_LIMITS,   // maxFeed or maxAccel is 0 on X or Y
    PQ_FULL,        // the ring is full; try again as it drains
};

// Queue a straight move to (x, y) in machine mm at `feed` mm/s. On an empty,
// idle ring the move starts from machinePos.
PlannerQueueResult plannerQueueLine(float x, float y, float feed);

// Blocks queued, the running one included.
int plannerQueueDepth();
