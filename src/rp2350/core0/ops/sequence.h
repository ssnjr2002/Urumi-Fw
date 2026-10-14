#pragma once
#include <stdint.h>
#include "leg.h"

// sequence.h — the sequence level: what was asked for, run as steps
// (docs/plans/machine-model.md, Levels).
//
// A sequence arms its first step, then starts here; the driver is the
// runner's one reader. A leg that fails fails the sequence (homingFail); one
// that succeeds goes to leg(), which may arm that node's next leg in the step
// (runner.h, threads). At each step end, or when a wait ends, the driver
// calls next(), which runs any instant work and arms the next step (or
// waits), or ends the sequence.
// A sequence has no context of its own: it runs in the one it started in.
//
// Closing or aborting a level tears down the levels below it: the teardown and
// contextClose abort the sequence, which cuts its step and calls abort().

enum SequenceId : uint8_t {
    SEQ_NONE     = 0,
    SEQ_LEG      = 1,   // one raw leg, in the manual homing context
    SEQ_TRAVERSE = 2,   // one leg that keeps the origin, outside a context
    SEQ_HOME     = 3,
    SEQ_RECOVERY = 4,
    SEQ_JOGTO    = 5,
};

// What ended a step or a wait, as next() is told. A wait's event is tied to
// the command that posts it.
enum SequenceEnd : uint8_t {
    SEQ_END_STEP    = 0,   // the step's last leg ended
    SEQ_END_TIMEOUT = 1,   // the wait timed out
};

struct Sequence {
    uint8_t id;                    // SequenceId
    // Run instant work and arm the next step, or sequenceWait. False when the
    // sequence is done. A failure faults (homingAbandon), which ends it.
    bool (*next)(uint8_t end);     // SequenceEnd
    // Clean up after a teardown; the step is already cut. May be null.
    void (*abort)(void);
    // A leg of the step succeeded: arm its node's next leg, or nothing. A
    // failure faults, which ends the sequence. May be null.
    void (*leg)(const LegEnd& end);
};

// Make `s` the live sequence. Its first step is armed already, or it waits.
void sequenceStart(const Sequence* s);

// Clear the level with no abort(): a start whose first step was refused.
void sequenceEnd(void);

// Cut the live step (abort its legs), end any wait and call abort().
void sequenceAbort(void);

// From next(): wait `ms` for nothing but the timeout.
void sequenceWait(uint32_t ms);

// The step just queued is RING: it ends when the ring drains.
void sequenceRing(void);

// The observer: Core 1's ring ran dry and nothing is left in it. Ends a RING
// step; ignored otherwise.
void sequenceRingDrained(void);

// Pop the runner's events and advance the live sequence; time out its wait.
// Call from the Core 0 loop after runnerTick.
void sequenceTick(void);

bool    sequenceLive(void);
uint8_t sequenceId(void);        // SequenceId
bool    sequenceWaiting(void);
