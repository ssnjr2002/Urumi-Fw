/**
 * lineTo (src/rp2350/core0/controller/seq/steps.cpp) against a faked planner:
 * a queued line is a RING step, a head already there queues nothing, a
 * refusal comes back as its text, and A rides a lock park to 0.
 */

#include "doctest.h"
#include <string.h>

#include "../../src/rp2350/core0/controller/seq/steps.cpp"

// ── fakes ───────────────────────────────────────────────────────────────────

namespace {
MachineCfg cfg;
float      at[4];
bool       jogFromOk;
PlannerQueueResult queueResult;
int        queued, rings;
bool       hasA, aZeroed;
CfgAxis    axisA;

void reset() {
    at[0] = at[1] = at[2] = at[3] = 0;
    jogFromOk = true;
    queueResult = PQ_OK;
    queued = rings = 0;
    hasA = true;
    aZeroed = false;
    axes_homed = 0x0f;
}
}  // namespace

volatile uint8_t axes_homed;
const CfgAxis* framesAxis(uint8_t k) { return k == SLOT_A && hasA ? &axisA : nullptr; }

bool machineCfgValid() { return true; }
const MachineCfg& machineCfg() { return cfg; }
float configParkPos(const CfgAxis&) { return 0; }
int32_t originTarget(uint8_t, int32_t s) { return s; }
const char* homingArmPark(uint8_t, bool, int32_t, uint16_t, uint16_t, uint16_t) {
    return nullptr;
}
void homingAbandon(uint8_t, uint8_t) {}
bool originValid(uint8_t) { return true; }
bool nodeMachineSteps(uint8_t, int32_t*) { return false; }
uint16_t busExcluded() { return 0; }
uint16_t busMute() { return 0; }
RpcResult rpcNodeCmd(uint8_t, uint8_t, uint8_t) { return RPC_OK; }
const char* rpcResultText(RpcResult) { return ""; }
void sequenceRing(void) { rings++; }

bool plannerJogFrom(float a[4], uint8_t) {
    for (int i = 0; i < 4; i++) a[i] = at[i];
    return jogFromOk;
}
PlannerQueueResult plannerQueueLine(float, float, float, uint8_t, bool aZero) {
    aZeroed = aZero;
    if (queueResult == PQ_OK) queued++;
    return queueResult;
}

// ── tests ───────────────────────────────────────────────────────────────────

TEST_CASE("a line is queued as a RING step") {
    reset();
    bool q;
    CHECK(lineTo(10, 20, 50, &q) == nullptr);
    CHECK(q);
    CHECK(queued == 1);
    CHECK(rings == 1);
}

TEST_CASE("a head already there queues nothing, without a refusal") {
    reset();
    at[0] = 10; at[1] = 20;
    bool q = true;
    CHECK(lineTo(10, 20, 50, &q) == nullptr);
    CHECK_FALSE(q);
    CHECK(rings == 0);
}

TEST_CASE("a refused line returns its refusal") {
    reset();
    queueResult = PQ_SOFT_LIMIT;
    bool q = true;
    const char* why = lineTo(10, 20, 50, &q);
    REQUIRE(why != nullptr);
    CHECK(strcmp(why, "soft_limit") == 0);
    CHECK_FALSE(q);
    CHECK(rings == 0);

    reset();
    jogFromOk = false;
    why = lineTo(10, 20, 50, &q);
    REQUIRE(why != nullptr);
    CHECK(strcmp(why, "bad_state") == 0);
}

TEST_CASE("a lock park takes A to 0, even with XY there") {
    reset();
    bool q;
    at[3] = 30;
    CHECK(lineTo(0, 0, 50, &q, true) == nullptr);
    CHECK(q);
    CHECK(aZeroed);
    reset();
    CHECK(lineTo(0, 0, 50, &q, true) == nullptr);   // A at the lock already
    CHECK_FALSE(q);
}

TEST_CASE("a lock park: XY only without A, refused with A unhomed") {
    reset();
    bool q;
    hasA = false;
    CHECK(lineTo(10, 0, 50, &q, true) == nullptr);
    CHECK_FALSE(aZeroed);
    reset();
    axes_homed = 0x07;
    CHECK(strcmp(lineTo(10, 0, 50, &q, true), "a_not_homed") == 0);
    CHECK_FALSE(q);
    CHECK(lineTo(10, 0, 50, &q) == nullptr);   // a plain line does not ask
}
