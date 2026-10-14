// sequence.cpp — see sequence.h.

#include <Arduino.h>
#include <stdio.h>
#include "sequence.h"
#include "runner.h"
#include "leg_effects.h"
#include "homing.h"            // homingFail
#include "machine.h"           // faultSet

static const Sequence* live;
static char     failWhy[48];
static uint8_t  abortedId;     // the last sequence aborted since a start
static bool     waiting;
static bool     ring;          // the live step is RING
static uint32_t waitFrom;
static uint32_t waitMs;

void sequenceStart(const Sequence* s) {
    live      = s;
    waiting   = false;
    ring      = false;
    abortedId = SEQ_NONE;
}

void sequenceEnd(void) {
    live    = nullptr;
    waiting = false;
    ring    = false;
}

void sequenceAbort(void) {
    if (!live) return;
    const Sequence* s = live;
    if (runnerLive()) {
        LegEnd ends[LEG_MAX];
        runnerAbortAll(ends);
    }
    sequenceEnd();
    abortedId = s->id;
    if (s->abort) s->abort();
}

void sequenceFail(const char* why) {
    // Copied first: the text may be refuse()'s buffer, which the teardown reuses.
    snprintf(failWhy, sizeof failWhy, "%s", why);
    faultSet(F_SEQUENCE);
}

const char* sequenceFailWhy(void) { return failWhy; }

void sequenceWait(uint32_t ms) {
    waiting  = true;
    waitFrom = millis();
    waitMs   = ms;
}

void sequenceRing(void) { ring = true; }

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

void sequenceRingDrained(void) {
    if (!live || !ring) return;
    ring = false;
    advance(SEQ_END_STEP);
}

bool sequenceLive(void) { return live != nullptr; }

uint8_t sequenceId(void) { return live ? live->id : (uint8_t)SEQ_NONE; }

uint8_t sequenceAbortedId(void) { return abortedId; }

bool sequenceWaiting(void) { return live && waiting; }
