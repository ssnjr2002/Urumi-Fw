/**
 * The leg runner (src/rp2350/core0/ops/runner.cpp) against a scripted leg
 * layer: steps open and close, legs join the live step, ends reach the
 * mailbox in order, and a cut step's events are never popped.
 */

#include "doctest.h"
#include <cstring>

// Unity build: runner.cpp includes only leg.h and leg_effects.h, which are
// native-clean; both are faked below.
#include "../../src/rp2350/core0/ops/runner.cpp"

// ── fake leg layer ──────────────────────────────────────────────────────────
// A leg is a node and the end it will report when `finish` names it.

namespace {
struct FakeLeg {
    bool    armed;
    LegEnd  end;
};
FakeLeg     fake[LEG_MAX];
int         due = -1;           // the slot legPoll reports next, or -1
const char* refuseNext;         // the next arm's refusal

// Each effect is logged as the intent and node it was applied to; `verdict`
// says whether it came through legEffectEnd.
struct Effect { uint8_t intent; uint8_t node; bool verdict; };
Effect      effects[16];
int         effectCount;
#define OWNER 5
uint8_t     heldState = OWNER;  // the state legOwnerHolds sees

void reset() {
    std::memset(fake, 0, sizeof fake);
    due = -1;
    refuseNext = nullptr;
    heldState = OWNER;
    LegEnd ends[LEG_MAX];
    runnerDropAll(ends);
    effectCount = 0;
}

const char* arm(uint8_t node, bool park = false) {
    if (refuseNext) { const char* w = refuseNext; refuseNext = nullptr; return w; }
    for (FakeLeg& f : fake) {
        if (f.armed) continue;
        f = {};
        f.armed    = true;
        f.end.node = node;
        f.end.park = park;
        return nullptr;
    }
    return "busy";
}

int slotOf(uint8_t node) {
    for (int i = 0; i < LEG_MAX; i++)
        if (fake[i].armed && fake[i].end.node == node) return i;
    return -1;
}

// Make `node`'s leg end on the next poll.
void finish(uint8_t node, uint8_t why = LEGFAIL_NONE) {
    due = slotOf(node);
    REQUIRE(due >= 0);
    fake[due].end.failWhy = why;
}

uint8_t takeAll(LegEnd* ends, uint8_t why) {
    uint8_t n = 0;
    for (FakeLeg& f : fake) {
        if (!f.armed) continue;
        f.armed = false;
        ends[n] = f.end;
        ends[n++].failWhy = why;
    }
    return n;
}
}  // namespace

const char* legArm(uint8_t node, uint8_t, uint8_t, bool, uint16_t, uint16_t,
                   uint16_t, uint32_t, bool* intentMismatch) {
    if (intentMismatch) *intentMismatch = false;
    return arm(node);
}
const char* legArmPark(uint8_t node, int32_t, uint16_t, uint16_t, uint16_t) {
    return arm(node, true);
}
const char* legArmDummy(bool, uint32_t) { return arm(0); }

LegPoll legPoll(LegEnd* end) {
    if (due < 0) return LEG_RUNNING;
    FakeLeg& f = fake[due];
    due = -1;
    f.armed = false;
    *end = f.end;
    return f.end.failWhy == LEGFAIL_NONE ? LEG_DONE : LEG_FAILED;
}

const char* legAbort(uint8_t node, LegEnd* end, bool* released) {
    *released = false;
    const int i = slotOf(node);
    if (i < 0) return nullptr;
    fake[i].armed = false;
    *end = fake[i].end;
    end->failWhy = LEGFAIL_ABORTED;
    *released = true;
    return nullptr;
}

uint8_t legAbortAll(LegEnd* ends) { return takeAll(ends, LEGFAIL_ABORTED); }
uint8_t legDropAll(LegEnd* ends)  { return takeAll(ends, LEGFAIL_NONE); }

bool legAny(void) {
    for (const FakeLeg& f : fake) if (f.armed) return true;
    return false;
}

bool legActive(uint8_t node) { return slotOf(node) >= 0; }

// ── fake effects ────────────────────────────────────────────────────────────

void legEffectEnd(uint8_t intent, const LegEnd& e)     { effects[effectCount++] = { intent, e.node, true }; }
void legEffectRelease(uint8_t intent, const LegEnd& e) { effects[effectCount++] = { intent, e.node, false }; }
bool legOwnerHolds(uint8_t owner) { return heldState == owner; }

// ── helpers ─────────────────────────────────────────────────────────────────

static const char* seek(uint8_t node, uint8_t intent = LEG_SEEK) {
    return runnerArm(OWNER, intent, node, 0, 0, false, 0, 0, 0, 0);
}

static int drain(RunEvent* out, int max) {
    int n = 0;
    while (n < max && runnerPop(OWNER, &out[n])) n++;
    return n;
}

// ── tests ───────────────────────────────────────────────────────────────────

TEST_CASE("a step opens on the first leg and ends with its last") {
    reset();
    CHECK_FALSE(runnerLive());
    CHECK(seek(3) == nullptr);
    CHECK(seek(4) == nullptr);
    CHECK(runnerLive());

    RunEvent ev[4];
    runnerTick();
    CHECK(drain(ev, 4) == 0);

    finish(3);
    runnerTick();
    REQUIRE(drain(ev, 4) == 1);
    CHECK(ev[0].kind == RUN_LEG_END);
    CHECK(ev[0].end.node == 3);
    CHECK(runnerLive());

    finish(4);
    runnerTick();
    REQUIRE(drain(ev, 4) == 2);
    CHECK(ev[0].kind == RUN_LEG_END);
    CHECK(ev[0].end.node == 4);
    CHECK(ev[1].kind == RUN_STEP_END);
    CHECK_FALSE(runnerLive());
}

TEST_CASE("a refused arm opens no step") {
    reset();
    refuseNext = "node 3 nak";
    CHECK(std::strcmp(seek(3), "node 3 nak") == 0);
    CHECK_FALSE(runnerLive());
}

TEST_CASE("a leg armed mid-step joins it") {
    reset();
    seek(3);
    finish(3);
    seek(4);                 // before the tick that ends node 3
    runnerTick();

    RunEvent ev[4];
    REQUIRE(drain(ev, 4) == 1);
    CHECK(ev[0].end.node == 3);
    CHECK(runnerLive());     // node 4 keeps the step open

    finish(4);
    runnerTick();
    REQUIRE(drain(ev, 4) == 2);
    CHECK(ev[1].kind == RUN_STEP_END);
}

TEST_CASE("a failed leg is posted with its cause") {
    reset();
    seek(3);
    seek(4);
    finish(3, LEGFAIL_BUDGET);
    runnerTick();

    RunEvent ev[4];
    REQUIRE(drain(ev, 4) == 1);
    CHECK(ev[0].end.failWhy == LEGFAIL_BUDGET);
    CHECK(runnerLive());
}

TEST_CASE("abort all cuts the step and discards its events") {
    reset();
    seek(3);
    seek(4);
    finish(3);
    runnerTick();            // node 3's end is posted, not yet popped

    LegEnd ends[LEG_MAX];
    CHECK(runnerAbortAll(ends) == 1);
    CHECK(ends[0].node == 4);
    CHECK(ends[0].failWhy == LEGFAIL_ABORTED);
    CHECK_FALSE(runnerLive());

    RunEvent ev[4];
    CHECK(drain(ev, 4) == 0);
}

TEST_CASE("drop all releases without a verdict") {
    reset();
    runnerArmPark(OWNER, 5, 100, 0, 0, 0);
    LegEnd ends[LEG_MAX];
    REQUIRE(runnerDropAll(ends) == 1);
    CHECK(ends[0].park);
    CHECK(ends[0].failWhy == LEGFAIL_NONE);
    CHECK_FALSE(runnerLive());
    REQUIRE(effectCount == 1);
    CHECK(effects[0].intent == LEG_PARK);
    CHECK_FALSE(effects[0].verdict);
}

TEST_CASE("aborting one leg returns it and posts only the step end") {
    reset();
    seek(3);
    seek(4);
    LegEnd e;
    bool released;
    CHECK(runnerAbort(3, &e, &released) == nullptr);
    CHECK(released);
    CHECK(e.failWhy == LEGFAIL_ABORTED);

    RunEvent ev[4];
    CHECK(drain(ev, 4) == 0);   // node 4 still runs
    CHECK(runnerAbort(4, &e, &released) == nullptr);
    REQUIRE(drain(ev, 4) == 1);
    CHECK(ev[0].kind == RUN_STEP_END);
    CHECK_FALSE(runnerLive());
}

TEST_CASE("an abort to a node with no leg releases nothing") {
    reset();
    seek(3);
    LegEnd e;
    bool released;
    CHECK(runnerAbort(9, &e, &released) == nullptr);
    CHECK_FALSE(released);
    CHECK(runnerLive());
}

TEST_CASE("each step has its own generation") {
    reset();
    seek(3);
    finish(3);
    runnerTick();
    RunEvent a[4];
    REQUIRE(drain(a, 4) == 2);

    seek(4);
    finish(4);
    runnerTick();
    RunEvent b[4];
    REQUIRE(drain(b, 4) == 2);
    CHECK(a[0].gen != b[0].gen);
}

TEST_CASE("the mailbox holds a full table's ends before it is drained") {
    reset();
    for (uint8_t n = 1; n <= LEG_MAX; n++) seek(n);
    for (uint8_t n = 1; n <= LEG_MAX; n++) { finish(n); runnerTick(); }

    RunEvent ev[RUN_MAILBOX + 1];
    REQUIRE(drain(ev, RUN_MAILBOX + 1) == LEG_MAX + 1);
    for (uint8_t n = 0; n < LEG_MAX; n++) CHECK(ev[n].end.node == n + 1);
    CHECK(ev[LEG_MAX].kind == RUN_STEP_END);
}

TEST_CASE("an ended leg's effects are applied by its intent before it is posted") {
    reset();
    seek(3, LEG_RETRACT);
    seek(4, LEG_SWEEP);
    finish(4, LEGFAIL_BUDGET);
    runnerTick();
    REQUIRE(effectCount == 1);
    CHECK(effects[0].intent == LEG_SWEEP);
    CHECK(effects[0].node == 4);
    CHECK(effects[0].verdict);

    finish(3);
    runnerTick();
    REQUIRE(effectCount == 2);
    CHECK(effects[1].intent == LEG_RETRACT);
    CHECK(effects[1].verdict);
}

TEST_CASE("aborts release without a verdict") {
    reset();
    seek(3);
    seek(4, LEG_RETRACT);
    LegEnd e;
    bool released;
    runnerAbort(3, &e, &released);
    REQUIRE(effectCount == 1);
    CHECK(effects[0].intent == LEG_SEEK);
    CHECK_FALSE(effects[0].verdict);

    LegEnd ends[LEG_MAX];
    runnerAbortAll(ends);
    REQUIRE(effectCount == 2);
    CHECK(effects[1].intent == LEG_RETRACT);
    CHECK_FALSE(effects[1].verdict);
}

TEST_CASE("a step whose owner lost the machine is dropped on the next tick") {
    reset();
    seek(3);
    seek(4);
    finish(3);               // would end, but the owner is gone first
    heldState = OWNER + 1;
    runnerTick();
    CHECK_FALSE(runnerLive());
    CHECK(effectCount == 2);
    CHECK_FALSE(effects[0].verdict);
    CHECK_FALSE(effects[1].verdict);

    RunEvent ev[4];
    CHECK(drain(ev, 4) == 0);
}

TEST_CASE("a joining leg runs under the live step's owner") {
    reset();
    seek(3);
    CHECK(runnerArm(OWNER + 1, LEG_SEEK, 4, 0, 0, false, 0, 0, 0, 0) == nullptr);
    runnerTick();
    CHECK(runnerLive());     // OWNER still holds; OWNER + 1 was never the owner
}

TEST_CASE("only the step's owner pops its events, each with its leg's intent") {
    reset();
    seek(3, LEG_SPAN);
    finish(3);
    runnerTick();
    RunEvent ev;
    CHECK_FALSE(runnerPop(OWNER + 1, &ev));   // not this state's step
    REQUIRE(runnerPop(OWNER, &ev));
    CHECK(ev.kind == RUN_LEG_END);
    CHECK(ev.intent == LEG_SPAN);
    REQUIRE(runnerPop(OWNER, &ev));
    CHECK(ev.kind == RUN_STEP_END);
}

TEST_CASE("a dummy's end is LEG_DUMMY whatever node 0 last ran") {
    reset();
    runnerArmDummy(OWNER, true, 0);
    LegEnd ends[LEG_MAX];
    for (FakeLeg& f : fake) if (f.armed) f.end.dummy = true;
    runnerDropAll(ends);
    REQUIRE(effectCount == 1);
    CHECK(effects[0].intent == LEG_DUMMY);
}
