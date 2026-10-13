#pragma once
#include <stdint.h>
#include "leg.h"

// runner.h — runs node legs in steps and posts how they end to a mailbox
// (docs/plans/leg-runner.md).
//
// A step is the batch of legs running together. The first leg armed with
// nothing live opens a step; legs armed while it runs join it. Each node's
// legs are a thread: arming opens it, and the end of a leg armed `last`
// closes it, so a sequence may arm a node's next leg on its previous one's
// end. The step ends when no leg runs and no thread is open. Arming is
// synchronous, so a refusal comes back from the arm call with nothing moved.
//
// The runner applies each end's physical effects (leg_effects.h), whoever
// armed the leg; what an end means is the live sequence's, its one reader.
//
// Ends are posted, never called back: the reader pops them on its own tick. A
// step that is dropped or aborted clears the box, so no event outlives its step.

enum RunEventKind : uint8_t {
    RUN_LEG_END,    // one leg ended; `end` says how
    RUN_STEP_END,   // the step's last leg has ended
};

struct RunEvent {
    uint8_t kind;    // RunEventKind
    uint8_t intent;  // RUN_LEG_END only: the leg's LegIntent
    LegEnd  end;     // RUN_LEG_END only
};

// Arm a leg into the live step, or open one. `intent` is a LegIntent
// (leg_effects.h); a park and a dummy imply theirs. `last`: no leg follows on
// this node's thread in the step. As legArm, legArmPark and legArmDummy.
const char* runnerArm(uint8_t intent, uint8_t node, bool last, uint8_t expectKind,
                      uint8_t dir, bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs, uint16_t rampSteps,
                      uint32_t maxSteps, bool* intentMismatch = nullptr);
const char* runnerArmPark(uint8_t node, bool last, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps);
const char* runnerArmDummy(bool last, bool succeed, uint32_t ms);

// Poll the live step's legs, apply each end's effects and post it, then the
// step's end. Call from the Core 0 loop.
void runnerTick(void);

// Take the oldest event. False when there is none.
bool runnerPop(RunEvent* ev);

// As legAbort: a released leg is released and comes back in `*end`, not as an
// event, and its thread closes.
const char* runnerAbort(uint8_t node, LegEnd* end, bool* released);

// End the step now, without posting: abort (legAbortAll) or release
// (legDropAll) every leg into `ends` (LEG_MAX entries), release each, and
// return how many. Pending events are discarded.
uint8_t runnerAbortAll(LegEnd* ends);
uint8_t runnerDropAll(LegEnd* ends);

// True while a step runs.
bool runnerLive(void);
