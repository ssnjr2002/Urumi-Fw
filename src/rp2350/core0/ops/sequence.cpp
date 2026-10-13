// sequence.cpp — see sequence.h.

#include <Arduino.h>
#include "sequence.h"
#include "runner.h"
#include "leg_effects.h"
#include "homing.h"            // homingFail

static const Sequence* live;
static bool     waiting;
static uint32_t waitFrom;
static uint32_t waitMs;

void sequenceStart(const Sequence* s) {
    live    = s;
    waiting = false;
}

void sequenceEnd(void) {
    live    = nullptr;
    waiting = false;
}

void sequenceAbort(void) {
    if (!live) return;
    const Sequence* s = live;
    if (runnerLive()) {
        LegEnd ends[LEG_MAX];
        runnerAbortAll(ends);
    }
    sequenceEnd();
    if (s->abort) s->abort();
}

void sequenceWait(uint32_t ms) {
    waiting  = true;
    waitFrom = millis();
    waitMs   = ms;
}

static void advance(uint8_t end) {
    const Sequence* s = live;
    const bool more = s->next(end);
    // A failure's teardown already ended it.
    if (live != s) return;
    if (!more) sequenceEnd();
}

void sequenceTick(void) {
    RunEvent ev;
    while (runnerPop(&ev)) {
        if (!live) continue;   // every step has a sequence; drain regardless
        if (ev.kind == RUN_STEP_END) { advance(SEQ_END_STEP); continue; }
        if (legEndFailed(ev.intent, ev.end)) homingFail(ev.end);
        else if (live->leg) live->leg(ev.end);
    }
    if (live && waiting && millis() - waitFrom >= waitMs) {
        waiting = false;
        advance(SEQ_END_TIMEOUT);
    }
}

bool sequenceLive(void) { return live != nullptr; }

uint8_t sequenceId(void) { return live ? live->id : (uint8_t)SEQ_NONE; }

bool sequenceWaiting(void) { return live && waiting; }
