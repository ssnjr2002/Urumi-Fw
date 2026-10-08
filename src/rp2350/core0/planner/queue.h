#pragma once
#include <stdint.h>

// queue.h — Core 0's side of planner motion: lines and Béziers into the shared
// ring.
//
// Core 1 runs what is queued (core1/emit/follower.cpp). Lines and Béziers are
// jogs, run as STATE_JOGGING; records are a job, run as STATE_RUNNING. Accepted
// in IDLE and while motion of the same kind runs; refused while the ring holds
// the other kind, and once a pause or abort is requested.

enum PlannerQueueResult : uint8_t {
    PQ_OK = 0,
    PQ_BAD_STATE,   // not IDLE or running motion of this kind, or stopping
    PQ_NO_CONFIG,   // no valid machine config
    PQ_NO_LIMITS,   // maxFeed or maxAccel is 0 on an axis the move needs
    PQ_FULL,        // the ring is full; try again as it drains
    PQ_BAD_CURVE,   // a degenerate handle, a cusp, or an unfittable curve
    PQ_NO_FEED,     // a record before any `feed`
    PQ_SOFT_LIMIT,  // the bed mesh would take Z outside its soft range
};

namespace planner { struct Bezier; }

// Queue a straight move to (x, y) in machine mm at `feed` mm/s. On an empty,
// idle ring the move starts from machinePos.
// `reason` is the JoggingReason it runs as; it joins only a ring of that kind.
// A continuous jog ends where the bed mesh would take Z outside its soft range;
// any other XY move is refused there (PQ_SOFT_LIMIT), as are Béziers and
// records.
PlannerQueueResult plannerQueueLine(float x, float y, float feed, uint8_t reason);

// Queue a move of slot k (SLOT_Z or SLOT_A) by `d` mm or degrees from where the
// queued moves end, at `feed` units/s. PQ_NO_LIMITS when no head binds the slot.
PlannerQueueResult plannerQueueAxis(uint8_t k, float d, float feed, uint8_t reason);

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

// Where the next jog starts on slots X, Y, Z, A, machine units (A in degrees,
// turns included): the ring's end, or machinePos on an empty, idle ring. False
// while another kind (JoggingReason) owns the ring.
bool plannerJogFrom(float at[4], uint8_t reason);

// Stop the jogs in the ring: brake and discard if Core 1 runs them, else empty
// the ring.
void plannerStopJog();

// Blocks queued, the running one included.
int plannerQueueDepth();
