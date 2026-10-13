/**
 * The sequence driver (src/rp2350/core0/ops/sequence.cpp) against a faked
 * runner: steps advance next(), a failed leg fails the sequence, a wait times
 * out, and an abort cuts the step before abort() runs.
 */

#include "doctest.h"
#include <stdint.h>

namespace {
uint32_t now;                   // millis()
}
uint32_t millis(void) { return now; }

// Unity build: sequence.cpp includes runner.h, leg_effects.h and homing.h,
// whose functions are faked below.
#include "../../src/rp2350/core0/ops/sequence.cpp"

// ── fake runner ─────────────────────────────────────────────────────────────

namespace {
RunEvent box[8];
int      boxHead, boxCount;
bool     stepLive;
int      stepCuts;

void post(uint8_t kind, uint8_t failWhy = LEGFAIL_NONE) {
    RunEvent& ev = box[(boxHead + boxCount++) % 8];
    ev = {};
    ev.kind        = kind;
    ev.intent      = LEG_SEEK;
    ev.end.failWhy = failWhy;
    if (kind == RUN_STEP_END) stepLive = false;
}

// ── fake failure: as the real one, it faults and the teardown aborts ────────
int fails;

// ── a scripted sequence ─────────────────────────────────────────────────────
// next() records what ended each step and plays `script`: 'S' arms a step,
// 'W' waits 100 ms, '.' ends.
const char* script;
uint8_t     ends[8];
int         nexts, aborts;

bool scriptNext(uint8_t end) {
    ends[nexts++] = end;
    switch (*script++) {
        case 'S': stepLive = true; return true;
        case 'W': sequenceWait(100); return true;
        default:  return false;
    }
}
void scriptAbort(void) { aborts++; }

const Sequence scripted = { SEQ_HOME, scriptNext, scriptAbort };

void reset(const char* s) {
    boxHead = boxCount = 0;
    stepLive = false;
    stepCuts = fails = nexts = aborts = 0;
    now = 0;
    sequenceEnd();
    script = s;
}

// Start `scripted` with its first step armed.
void start(const char* s) {
    reset(s);
    stepLive = true;
    sequenceStart(&scripted);
}
}  // namespace

bool runnerPop(RunEvent* ev) {
    if (!boxCount) return false;
    *ev = box[boxHead];
    boxHead = (boxHead + 1) % 8;
    boxCount--;
    return true;
}
bool runnerLive(void) { return stepLive; }
uint8_t runnerAbortAll(LegEnd*) {
    stepCuts++;
    stepLive = false;
    boxHead = boxCount = 0;
    return 0;
}
void homingFail(const LegEnd&) {
    fails++;
    LegEnd e[LEG_MAX];
    runnerAbortAll(e);
    sequenceAbort();
}

// ── tests ───────────────────────────────────────────────────────────────────

TEST_CASE("each step end advances the sequence until it is done") {
    start("S.");
    CHECK(sequenceLive());
    CHECK(sequenceId() == SEQ_HOME);
    sequenceTick();
    CHECK(nexts == 0);                     // nothing ended yet

    post(RUN_LEG_END);
    post(RUN_STEP_END);
    sequenceTick();
    REQUIRE(nexts == 1);
    CHECK(ends[0] == SEQ_END_STEP);
    CHECK(sequenceLive());                 // a second step armed

    post(RUN_STEP_END);
    sequenceTick();
    CHECK(nexts == 2);
    CHECK_FALSE(sequenceLive());
    CHECK(aborts == 0);                    // done, not aborted
}

TEST_CASE("a failed leg fails the sequence; its later events are gone") {
    start("S.");
    post(RUN_LEG_END, LEGFAIL_BUDGET);
    post(RUN_STEP_END);
    sequenceTick();
    CHECK(fails == 1);
    CHECK(nexts == 0);
    CHECK(aborts == 1);
    CHECK_FALSE(sequenceLive());
}

TEST_CASE("a wait ends on its timeout") {
    start("W.");
    post(RUN_STEP_END);
    sequenceTick();
    CHECK(sequenceWaiting());
    now = 99;
    sequenceTick();
    CHECK(nexts == 1);
    now = 100;
    sequenceTick();
    REQUIRE(nexts == 2);
    CHECK(ends[1] == SEQ_END_TIMEOUT);
    CHECK_FALSE(sequenceLive());
    CHECK_FALSE(sequenceWaiting());
}

TEST_CASE("an abort cuts the live step, then calls abort()") {
    start("S.");
    sequenceAbort();
    CHECK(stepCuts == 1);
    CHECK(aborts == 1);
    CHECK_FALSE(sequenceLive());
    sequenceAbort();                       // nothing live: a no-op
    CHECK(aborts == 1);
}

TEST_CASE("an abort mid-wait cuts no step and ends the wait") {
    start("W.");
    post(RUN_STEP_END);
    sequenceTick();
    sequenceAbort();
    CHECK(stepCuts == 0);
    CHECK(aborts == 1);
    CHECK_FALSE(sequenceWaiting());
    now = 1000;
    sequenceTick();
    CHECK(nexts == 1);                     // the dead wait never fires
}

TEST_CASE("a refused start ends with no abort()") {
    reset(".");
    sequenceStart(&scripted);
    sequenceEnd();
    CHECK_FALSE(sequenceLive());
    CHECK(aborts == 0);
}

TEST_CASE("events with no sequence are drained") {
    reset(".");
    post(RUN_LEG_END);
    post(RUN_STEP_END);
    sequenceTick();
    CHECK(boxCount == 0);
    CHECK(nexts == 0);
}
