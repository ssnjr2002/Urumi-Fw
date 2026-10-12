#include "runner.h"

// A tick posts at most a leg end and a step end, and the owner drains the box
// every tick; legs that join a running step can outnumber LEG_MAX over its
// life, so this bounds what is undrained, not what a step posts.
#define RUN_MAILBOX (LEG_MAX + 1)

static RunEvent box[RUN_MAILBOX];
static uint8_t  boxHead;    // next to pop
static uint8_t  boxCount;
static uint8_t  gen;        // the live step's
static bool     live;

static void post(uint8_t kind, const LegEnd* end) {
    // An owner that stopped draining: a bug, so stop here rather than lose an end.
    if (boxCount == RUN_MAILBOX) for (;;) {}
    RunEvent& ev = box[(boxHead + boxCount++) % RUN_MAILBOX];
    ev.kind = kind;
    ev.gen  = gen;
    if (end) ev.end = *end;
}

static void boxClear(void) {
    boxHead  = 0;
    boxCount = 0;
}

// A leg was armed: open a step if none runs.
static const char* joined(const char* why) {
    if (!why && !live) {
        live = true;
        gen++;
    }
    return why;
}

const char* runnerArm(uint8_t node, uint8_t expectKind, uint8_t dir, bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs, uint16_t rampSteps,
                      uint32_t maxSteps, bool* intentMismatch) {
    return joined(legArm(node, expectKind, dir, intendedRetract,
                         startUs, floorUs, rampSteps, maxSteps, intentMismatch));
}

const char* runnerArmPark(uint8_t node, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps) {
    return joined(legArmPark(node, target, startUs, floorUs, rampSteps));
}

const char* runnerArmDummy(bool succeed, uint32_t ms) {
    return joined(legArmDummy(succeed, ms));
}

// The step is over once its last leg is.
static void stepCheck(void) {
    if (!live || legAny()) return;
    live = false;
    post(RUN_STEP_END, nullptr);
}

void runnerTick(void) {
    if (!live) return;
    LegEnd e;
    if (legPoll(&e) != LEG_RUNNING) post(RUN_LEG_END, &e);
    stepCheck();
}

bool runnerPop(RunEvent* ev) {
    while (boxCount) {
        *ev = box[boxHead];
        boxHead = (uint8_t)((boxHead + 1) % RUN_MAILBOX);
        boxCount--;
        if (ev->gen == gen) return true;
    }
    return false;
}

const char* runnerAbort(uint8_t node, LegEnd* end, bool* released) {
    const char* why = legAbort(node, end, released);
    // The released end is the caller's; only the step's end is posted.
    if (*released) stepCheck();
    return why;
}

// Ends the step without a step end: the caller has the ends in hand.
static uint8_t stepCut(uint8_t n) {
    live = false;
    gen++;
    boxClear();
    return n;
}

uint8_t runnerAbortAll(LegEnd* ends) { return stepCut(legAbortAll(ends)); }

uint8_t runnerDropAll(LegEnd* ends) { return stepCut(legDropAll(ends)); }

bool runnerLive(void) { return live; }
