#pragma once
#include <stdint.h>

// queue.h — Core 0's side of planner motion: lines and Béziers into the shared
// ring.
//
// Core 1 runs what is queued (core1/emit/follower.cpp). Accepted in IDLE, and
// while planner motion runs; refused once a pause or abort is requested.

enum PlannerQueueResult : uint8_t {
    PQ_OK = 0,
    PQ_BAD_STATE,   // not IDLE or running planner motion, or stopping
    PQ_NO_CONFIG,   // no valid machine config
    PQ_NO_LIMITS,   // maxFeed or maxAccel is 0 on X or Y
    PQ_FULL,        // the ring is full; try again as it drains
    PQ_BAD_CURVE,   // a degenerate handle, a cusp, or an unfittable curve
    PQ_NO_FEED,     // a record before any `feed`
};

namespace planner { struct Bezier; }

// Queue a straight move to (x, y) in machine mm at `feed` mm/s. On an empty,
// idle ring the move starts from machinePos.
PlannerQueueResult plannerQueueLine(float x, float y, float feed);

// Queue a cubic Bézier from the end of the last move (machinePos on an empty,
// idle ring) through handles p1, p2 to p3, machine mm, at `feed` mm/s.
PlannerQueueResult plannerQueueBezier(float x1, float y1, float x2, float y2,
                                      float x3, float y3, float feed);

// Cut and travel feeds for streamed records, mm/s; held until reboot.
void plannerSetFeed(float cut, float travel);

// Queue a host-analysed record (checkBezier fills the rest of `b`). Contour
// framing: `start` opens a contour and travels to p0 if it is elsewhere; any
// other record must continue the ring's end; `end` closes the contour.
// PQ_BAD_CURVE for a failed check or broken framing.
PlannerQueueResult plannerQueueRecord(planner::Bezier& b, bool start, bool end);

// Forget an open contour: seqreset, abort, soft reset.
void plannerEndContour();

// Blocks queued, the running one included.
int plannerQueueDepth();
