#pragma once
#include <stdint.h>
#include "leg.h"

// runner.h — runs node legs in steps and posts how they end to a mailbox
// (docs/plans/leg-runner.md).
//
// A step is the batch of legs running together. The first leg armed with
// nothing live opens a step; legs armed while it runs join it. The step ends
// when its last leg does. Arming is synchronous, so a refusal comes back from
// the arm call with nothing moved.
//
// The runner applies each end's physical effects (leg_effects.h), whoever
// armed the leg; what an end means for the session is the owner's.
//
// Ends are posted, never called back: the owner pops them on its own tick.
// Every event carries the generation of the step it belongs to; a step that is
// dropped or aborted bumps it, so events of an older step are never popped.

enum RunEventKind : uint8_t {
    RUN_LEG_END,    // one leg ended; `end` says how
    RUN_STEP_END,   // the step's last leg has ended
};

struct RunEvent {
    uint8_t kind;   // RunEventKind
    uint8_t gen;
    LegEnd  end;    // RUN_LEG_END only
};

// Arm a leg into the live step, or open one under `owner` (a MachineState: the
// state the step runs in). A leg joining a live step runs under its owner.
// `intent` is a LegIntent (leg_effects.h); a park and a dummy imply theirs.
// As legArm, legArmPark and legArmDummy.
const char* runnerArm(uint8_t owner, uint8_t intent, uint8_t node, uint8_t expectKind,
                      uint8_t dir, bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs, uint16_t rampSteps,
                      uint32_t maxSteps, bool* intentMismatch = nullptr);
const char* runnerArmPark(uint8_t owner, uint8_t node, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps);
const char* runnerArmDummy(uint8_t owner, bool succeed, uint32_t ms);

// Poll the live step's legs, apply each end's effects and post it, then the
// step's end. A step whose owner no longer holds the machine is dropped
// instead: its legs released, nothing posted. Call from the Core 0 loop.
void runnerTick(void);

// Take the oldest event of the live generation. False when there is none.
bool runnerPop(RunEvent* ev);

// As legAbort: a released leg is released and comes back in `*end`, not as an
// event.
const char* runnerAbort(uint8_t node, LegEnd* end, bool* released);

// End the step now, without posting: abort (legAbortAll) or release
// (legDropAll) every leg into `ends` (LEG_MAX entries), release each, and
// return how many. Pending events are discarded.
uint8_t runnerAbortAll(LegEnd* ends);
uint8_t runnerDropAll(LegEnd* ends);

// True while a step runs.
bool runnerLive(void);
