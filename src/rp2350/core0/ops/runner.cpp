#include <string.h>
#include "runner.h"
#include "leg_effects.h"

// A tick posts at most a leg end and a step end, and the reader drains the box
// every tick.
#define RUN_MAILBOX (LEG_MAX + 1)

static RunEvent box[RUN_MAILBOX];
static uint8_t  boxHead;    // next to pop
static uint8_t  boxCount;
static bool     live;

// Each node's leg intent, indexed by node; a dummy is LEG_DUMMY by its flag.
static uint8_t  intentOf[256];
// Each node's thread: open, and whether its live leg is its last.
static bool     openOf[256];
static bool     lastOf[256];
static uint8_t  openCount;

static uint8_t intentFor(const LegEnd& e) {
    return e.dummy ? (uint8_t)LEG_DUMMY : intentOf[e.node];
}

static void post(uint8_t kind, const LegEnd* end, uint8_t intent) {
    // A reader that stopped draining: a bug, so stop here rather than lose an end.
    if (boxCount == RUN_MAILBOX) for (;;) {}
    RunEvent& ev = box[(boxHead + boxCount++) % RUN_MAILBOX];
    ev.kind   = kind;
    ev.intent = intent;
    if (end) ev.end = *end;
}

static void boxClear(void) {
    boxHead  = 0;
    boxCount = 0;
}

static void threadClose(uint8_t node) {
    if (!openOf[node]) return;
    openOf[node] = false;
    openCount--;
}

// A leg was armed: open its thread, and a step if none runs.
static const char* joined(const char* why, uint8_t node, uint8_t intent, bool last) {
    if (why) return why;
    intentOf[node] = intent;
    lastOf[node]   = last;
    if (!openOf[node]) { openOf[node] = true; openCount++; }
    live = true;
    return nullptr;
}

const char* runnerArm(uint8_t intent, uint8_t node, bool last, uint8_t expectKind,
                      uint8_t dir, bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs, uint16_t rampSteps,
                      uint32_t maxSteps, bool* intentMismatch) {
    return joined(legArm(node, expectKind, dir, intendedRetract,
                         startUs, floorUs, rampSteps, maxSteps, intentMismatch),
                  node, intent, last);
}

const char* runnerArmPark(uint8_t node, bool last, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps) {
    return joined(legArmPark(node, target, startUs, floorUs, rampSteps),
                  node, LEG_PARK, last);
}

const char* runnerArmDummy(bool last, bool succeed, uint32_t ms) {
    return joined(legArmDummy(succeed, ms), 0, LEG_DUMMY, last);
}

// The step is over once no leg runs and no thread waits for its next.
static void stepCheck(void) {
    if (!live || legAny() || openCount) return;
    live = false;
    post(RUN_STEP_END, nullptr, 0);
}

// Ends the step without a step end, releasing every leg in `ends`.
static uint8_t stepCut(LegEnd* ends, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) legEffectRelease(intentFor(ends[i]), ends[i]);
    memset(openOf, 0, sizeof openOf);
    openCount = 0;
    live = false;
    boxClear();
    return n;
}

void runnerTick(void) {
    if (!live) return;
    LegEnd e;
    if (legPoll(&e) != LEG_RUNNING) {
        const uint8_t intent = intentFor(e);
        legEffectEnd(intent, e);
        if (lastOf[e.node]) threadClose(e.node);
        post(RUN_LEG_END, &e, intent);
    }
    stepCheck();
}

bool runnerPop(RunEvent* ev) {
    if (!boxCount) return false;
    *ev = box[boxHead];
    boxHead = (uint8_t)((boxHead + 1) % RUN_MAILBOX);
    boxCount--;
    return true;
}

const char* runnerAbort(uint8_t node, LegEnd* end, bool* released) {
    const char* why = legAbort(node, end, released);
    // The released end is the caller's; only the step's end is posted.
    if (*released) {
        legEffectRelease(intentFor(*end), *end);
        threadClose(end->node);
        stepCheck();
    }
    return why;
}

uint8_t runnerAbortAll(LegEnd* ends) { return stepCut(ends, legAbortAll(ends)); }

uint8_t runnerDropAll(LegEnd* ends) { return stepCut(ends, legDropAll(ends)); }

bool runnerLive(void) { return live; }
