/**
 * lineTo (src/rp2350/core0/controller/seq/steps.cpp) against a faked planner:
 * a queued line is a RING step, a head already there queues nothing, and a
 * refusal comes back as its text.
 */

#include "doctest.h"
#include <string.h>

#include "../../src/rp2350/core0/controller/seq/steps.cpp"

// ── fakes ───────────────────────────────────────────────────────────────────

namespace {
MachineCfg cfg;
float      at[2];
bool       jogFromOk;
PlannerQueueResult queueResult;
int        queued, rings;

void reset() {
    at[0] = at[1] = 0;
    jogFromOk = true;
    queueResult = PQ_OK;
    queued = rings = 0;
}
}  // namespace

bool machineCfgValid() { return true; }
const MachineCfg& machineCfg() { return cfg; }
float configParkPos(const CfgAxis&) { return 0; }
int32_t originTarget(uint8_t, int32_t s) { return s; }
const char* homingArmPark(uint8_t, bool, int32_t, uint16_t, uint16_t, uint16_t) {
    return nullptr;
}
void homingAbandon(uint8_t, uint8_t) {}
void sequenceRing(void) { rings++; }

bool plannerJogFrom(float a[4], uint8_t) {
    a[0] = at[0]; a[1] = at[1]; a[2] = a[3] = 0;
    return jogFromOk;
}
PlannerQueueResult plannerQueueLine(float, float, float, uint8_t) {
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
