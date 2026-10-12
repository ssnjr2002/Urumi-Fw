#include "runner.h"
#include "leg_effects.h"

// A tick posts at most a leg end and a step end, and the owner drains the box
// every tick; legs that join a running step can outnumber LEG_MAX over its
// life, so this bounds what is undrained, not what a step posts.
#define RUN_MAILBOX (LEG_MAX + 1)

static RunEvent box[RUN_MAILBOX];
static uint8_t  boxHead;    // next to pop
static uint8_t  boxCount;
static uint8_t  gen;        // the live step's
static bool     live;
static uint8_t  owner;      // the state the live step runs in

// Each node's leg intent, indexed by node; a dummy is LEG_DUMMY by its flag.
static uint8_t  intentOf[256];

static uint8_t intentFor(const LegEnd& e) {
    return e.dummy ? (uint8_t)LEG_DUMMY : intentOf[e.node];
}

static void post(uint8_t kind, const LegEnd* end, uint8_t intent) {
    // An owner that stopped draining: a bug, so stop here rather than lose an end.
    if (boxCount == RUN_MAILBOX) for (;;) {}
    RunEvent& ev = box[(boxHead + boxCount++) % RUN_MAILBOX];
    ev.kind = kind;
    ev.gen    = gen;
    ev.intent = intent;
    if (end) ev.end = *end;
}

static void boxClear(void) {
    boxHead  = 0;
    boxCount = 0;
}

// A leg was armed: open a step if none runs. A leg joining a live step runs
// under that step's owner.
static const char* joined(const char* why, uint8_t own, uint8_t node, uint8_t intent) {
    if (why) return why;
    intentOf[node] = intent;
    if (!live) {
        live  = true;
        owner = own;
        gen++;
    }
    return nullptr;
}

const char* runnerArm(uint8_t own, uint8_t intent, uint8_t node, uint8_t expectKind,
                      uint8_t dir, bool intendedRetract,
                      uint16_t startUs, uint16_t floorUs, uint16_t rampSteps,
                      uint32_t maxSteps, bool* intentMismatch) {
    return joined(legArm(node, expectKind, dir, intendedRetract,
                         startUs, floorUs, rampSteps, maxSteps, intentMismatch),
                  own, node, intent);
}

const char* runnerArmPark(uint8_t own, uint8_t node, int32_t target, uint16_t startUs,
                          uint16_t floorUs, uint16_t rampSteps) {
    return joined(legArmPark(node, target, startUs, floorUs, rampSteps),
                  own, node, LEG_PARK);
}

const char* runnerArmDummy(uint8_t own, bool succeed, uint32_t ms) {
    return joined(legArmDummy(succeed, ms), own, 0, LEG_DUMMY);
}

// The step is over once its last leg is.
static void stepCheck(void) {
    if (!live || legAny()) return;
    live = false;
    post(RUN_STEP_END, nullptr, 0);
}

// Ends the step without a step end, releasing every leg in `ends`.
static uint8_t stepCut(LegEnd* ends, uint8_t n) {
    for (uint8_t i = 0; i < n; i++) legEffectRelease(intentFor(ends[i]), ends[i]);
    live = false;
    gen++;
    boxClear();
    return n;
}

void runnerTick(void) {
    if (!live) return;
    // Something else has taken the machine -- `stop` sets ESTOP, Core 1 folds
    // it to ALARM. Drop the legs rather than fight for them: leaving them armed
    // would fire timeouts later, at a moment with nothing to do with the owner.
    if (!legOwnerHolds(owner)) {
        LegEnd ends[LEG_MAX];
        stepCut(ends, legDropAll(ends));
        return;
    }
    LegEnd e;
    if (legPoll(&e) != LEG_RUNNING) {
        const uint8_t intent = intentFor(e);
        legEffectEnd(intent, e);
        post(RUN_LEG_END, &e, intent);
    }
    stepCheck();
}

bool runnerPop(uint8_t own, RunEvent* ev) {
    if (own != owner) return false;
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
    if (*released) {
        legEffectRelease(intentFor(*end), *end);
        stepCheck();
    }
    return why;
}

uint8_t runnerAbortAll(LegEnd* ends) { return stepCut(ends, legAbortAll(ends)); }

uint8_t runnerDropAll(LegEnd* ends) { return stepCut(ends, legDropAll(ends)); }

bool runnerLive(void) { return live; }
