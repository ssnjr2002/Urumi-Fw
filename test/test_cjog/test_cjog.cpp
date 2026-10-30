/**
 * The continuous jog (src/rp2350/core0/ops/cjog.cpp) against a faked planner
 * queue, config and machine state: packet checks, the line it queues, repeats,
 * the deadman, a direction change held for rest, and the replies.
 */

#include "doctest.h"
#include <string.h>

#include "../../src/rp2350/core0/ops/cjog.cpp"

// ── fakes ───────────────────────────────────────────────────────────────────

volatile uint8_t ringOwner;
volatile uint8_t axes_homed;
volatile bool    plannerActive, abortRequested;

namespace {
MachineCfg cfg;
bool       cfgValid;
CfgAxis    axes[4];
bool       hasAxis[4];
float      lo[4], hi[4];             // soft range; hi < lo means none
float      at[4];                    // where a jog starts
bool       jogFromOk;
int        depth;                    // blocks in the ring
const char* admit;                   // machineAdmit's answer when the ring is idle
PlannerQueueResult queueResult;

// The last move queued.
struct Move { bool axis; uint8_t k; float x, y, d, feed; uint8_t reason; };
Move moves[8];
int  moveCount, jogStops;

CfgAxis linear(float travel, float feed) {
    CfgAxis a;
    memset(&a, 0, sizeof a);
    a.stepsPerUnit   = 100;
    a.maxTravel      = travel;
    a.jogFeed        = feed;
    a.jogFeedUnhomed = feed / 4;
    a.softLimits     = true;
    return a;
}

void reset() {
    cjogStop();
    memset(&cfg, 0, sizeof cfg);
    cfgValid = true;
    for (uint8_t k = 0; k < 4; k++) {
        axes[k] = linear(100, 40);
        hasAxis[k] = true;
        lo[k] = 0;
        hi[k] = 100;
        at[k] = 0;
    }
    axes[SLOT_A].softLimits = false;
    axes_homed = 0x0f;
    ringOwner = RING_JOB;
    plannerActive = abortRequested = false;
    jogFromOk = true;
    depth = 0;
    admit = nullptr;
    queueResult = PQ_OK;
    moveCount = jogStops = 0;
}

const int8_t XP[4]   = { 1, 0, 0, 0 };
const int8_t XN[4]   = {-1, 0, 0, 0 };
const int8_t YP[4]   = { 0, 1, 0, 0 };
const int8_t XYP[4]  = { 1, 1, 0, 0 };
const int8_t ZN[4]   = { 0, 0,-1, 0 };
const int8_t AP[4]   = { 0, 0, 0, 1 };
const int8_t NONE[4] = { 0, 0, 0, 0 };
}  // namespace

bool machineCfgValid()               { return cfgValid; }
const MachineCfg& machineCfg()       { return cfg; }
const CfgAxis* framesAxis(uint8_t k) { return hasAxis[k] ? &axes[k] : nullptr; }
bool configAxisRange(const CfgAxis& a, float* l, float* h) {
    const int k = (int)(&a - axes);
    if (hi[k] < lo[k]) return false;
    *l = lo[k];
    *h = hi[k];
    return true;
}
bool framesTipRange(uint8_t k, float* l, float* h, float) {
    return framesAxis(k) && configAxisRange(*framesAxis(k), l, h);
}
const char* framesCheckXY(float, float) { return nullptr; }
float meshLatchedOffset(float, float) { return 0; }
// A running ring is a live step.
const char* machineAdmit(uint8_t kind) {
    CHECK(kind == (K_STEP | K_IN_TOOL));
    return depth != 0 || plannerActive ? "busy" : admit;
}

bool plannerJogFrom(float out[4], uint8_t reason) {
    if (!jogFromOk) return false;
    CHECK(reason == RING_JOG_CONT);
    memcpy(out, at, sizeof at);
    return true;
}
PlannerQueueResult plannerQueueLine(float x, float y, float feed, uint8_t reason, bool) {
    if (queueResult != PQ_OK) return queueResult;
    moves[moveCount++] = { false, 0, x, y, 0, feed, reason };
    depth = 1;
    ringOwner = reason;
    return PQ_OK;
}
PlannerQueueResult plannerQueueAxis(uint8_t k, float d, float feed, uint8_t reason) {
    if (queueResult != PQ_OK) return queueResult;
    moves[moveCount++] = { true, k, 0, 0, d, feed, reason };
    depth = 1;
    ringOwner = reason;
    return PQ_OK;
}
void plannerStopJog() { jogStops++; }
int  plannerQueueDepth() { return depth; }

// ── packet checks ───────────────────────────────────────────────────────────

TEST_CASE("a bad direction or speed is BAD_STATE, two axis sets MIXED_AXES") {
    reset();
    const int8_t bad[4] = { 2, 0, 0, 0 };
    CHECK(cjogPacket(bad, 64, 0) == MSEG_NACK_BAD_STATE);
    CHECK(cjogPacket(XP, 0, 0) == MSEG_NACK_BAD_STATE);
    const int8_t xz[4] = { 1, 0, 1, 0 };
    CHECK(cjogPacket(xz, 64, 0) == MSEG_NACK_MIXED_AXES);
    const int8_t za[4] = { 0, 0, 1, 1 };
    CHECK(cjogPacket(za, 64, 0) == MSEG_NACK_MIXED_AXES);
    CHECK(moveCount == 0);
}

// ── the line it queues ──────────────────────────────────────────────────────

TEST_CASE("a homed XY jog runs to the soft end at jogFeed × speed/64") {
    reset();
    at[0] = 30;
    at[1] = 20;
    CHECK(cjogPacket(XP, 32, 0) == CJOG_ACK);
    REQUIRE(moveCount == 1);
    CHECK_FALSE(moves[0].axis);
    CHECK(moves[0].x == doctest::Approx(100));
    CHECK(moves[0].y == doctest::Approx(20));
    CHECK(moves[0].feed == doctest::Approx(20));
    CHECK(moves[0].reason == RING_JOG_CONT);

    reset();
    at[0] = 30;
    CHECK(cjogPacket(XN, 64, 0) == CJOG_ACK);
    CHECK(moves[0].x == doctest::Approx(0));
}

TEST_CASE("a diagonal stops at the nearer end, at the slower feed") {
    reset();
    at[0] = 30;                     // 70 to go
    at[1] = 80;                     // 20 to go
    axes[1].jogFeed = 10;
    CHECK(cjogPacket(XYP, 64, 0) == CJOG_ACK);
    CHECK(moves[0].x == doctest::Approx(50));
    CHECK(moves[0].y == doctest::Approx(100));
    CHECK(moves[0].feed == doctest::Approx(10));
}

TEST_CASE("the feed is capped at maxFeed") {
    reset();
    axes[0].maxFeed = 15;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(moves[0].feed == doctest::Approx(15));
}

TEST_CASE("unhomed: refused without jogUnhomed, else maxTravel at jogFeedUnhomed") {
    reset();
    axes_homed = 0;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BAD_STATE);
    CHECK(moveCount == 0);

    cfg.jogUnhomed = true;
    at[0] = 30;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(moves[0].x == doctest::Approx(130));      // no soft range unhomed
    CHECK(moves[0].feed == doctest::Approx(10));
}

TEST_CASE("without softLimits a homed axis runs maxTravel") {
    reset();
    axes[0].softLimits = false;
    at[0] = 30;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(moves[0].x == doctest::Approx(130));
}

TEST_CASE("Z and A queue an axis move; A runs one turn") {
    reset();
    at[SLOT_Z] = 40;
    CHECK(cjogPacket(ZN, 64, 0) == CJOG_ACK);
    CHECK(moves[0].axis);
    CHECK(moves[0].k == SLOT_Z);
    CHECK(moves[0].d == doctest::Approx(-40));

    reset();
    CHECK(cjogPacket(AP, 64, 0) == CJOG_ACK);
    CHECK(moves[0].k == SLOT_A);
    CHECK(moves[0].d == doctest::Approx(360));
}

TEST_CASE("no room left, or the mesh leaving Z none, is SOFT_LIMIT") {
    reset();
    at[0] = 100;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_SOFT_LIMIT);
    CHECK(moveCount == 0);

    reset();
    queueResult = PQ_SOFT_LIMIT;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_SOFT_LIMIT);
}

TEST_CASE("no config, no axis, or a ring it cannot join is BAD_STATE") {
    reset();
    cfgValid = false;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BAD_STATE);
    reset();
    hasAxis[0] = false;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BAD_STATE);
    reset();
    jogFromOk = false;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BAD_STATE);
    reset();
    queueResult = PQ_FULL;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BAD_STATE);
}

// ── holding, stopping ───────────────────────────────────────────────────────

TEST_CASE("a repeat renews the deadman and queues nothing") {
    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(cjogPacket(XP, 64, 100) == CJOG_NO_REPLY);
    CHECK(cjogTick(200) == CJOG_NO_REPLY);           // 100 ms since the repeat
    CHECK(jogStops == 0);
    CHECK(moveCount == 1);
}

TEST_CASE("the deadman stops a silent jog") {
    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(cjogTick(CJOG_DEADMAN_MS) == CJOG_NO_REPLY);
    CHECK(jogStops == 0);
    CHECK(cjogTick(CJOG_DEADMAN_MS + 1) == CJOG_NO_REPLY);
    CHECK(jogStops == 1);
    CHECK(cjogTick(CJOG_DEADMAN_MS + 50) == CJOG_NO_REPLY);
    CHECK(jogStops == 1);                            // once
}

TEST_CASE("all-zero directions and the stop byte stop at once") {
    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(cjogPacket(NONE, 64, 10) == CJOG_NO_REPLY);
    CHECK(jogStops == 1);

    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    cjogStop();
    CHECK(jogStops == 1);
}

TEST_CASE("a jog that ran to its end is not stopped again") {
    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    depth = 0;                                       // the line finished
    CHECK(cjogTick(10) == CJOG_NO_REPLY);
    cjogStop();
    CHECK(jogStops == 0);
}

// ── a direction change, held for rest ───────────────────────────────────────

TEST_CASE("a new direction stops the jog and starts once at rest") {
    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(cjogPacket(YP, 64, 10) == CJOG_NO_REPLY);
    CHECK(jogStops == 1);
    CHECK(cjogTick(20) == CJOG_NO_REPLY);            // still braking
    CHECK(cjogPacket(YP, 64, 30) == CJOG_NO_REPLY);  // repeats while braking are held
    CHECK(jogStops == 1);
    CHECK(moveCount == 1);

    depth = 0;
    CHECK(cjogTick(40) == CJOG_ACK);
    REQUIRE(moveCount == 2);
    CHECK(moves[1].y == doctest::Approx(100));
    CHECK(cjogPacket(YP, 64, 50) == CJOG_NO_REPLY);  // now a repeat
    CHECK(moveCount == 2);
}

TEST_CASE("a packet that starts the held direction itself drops the held start") {
    reset();
    CHECK(cjogPacket(XP, 64, 0) == CJOG_ACK);
    CHECK(cjogPacket(YP, 64, 10) == CJOG_NO_REPLY);
    depth = 0;                                       // at rest before the tick
    CHECK(cjogPacket(YP, 64, 20) == CJOG_ACK);
    CHECK(moveCount == 2);
    depth = 0;                                       // ran to its end, still held
    CHECK(cjogPacket(YP, 64, 30) == CJOG_NO_REPLY);
    CHECK(cjogTick(40) == CJOG_NO_REPLY);
    CHECK(moveCount == 2);
}

TEST_CASE("a held start waits for an abort to finish") {
    reset();
    depth = 1;
    ringOwner = RING_JOG_STEP;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_NO_REPLY);
    depth = 0;
    abortRequested = true;
    CHECK(cjogTick(20) == CJOG_NO_REPLY);
    abortRequested = false;
    CHECK(cjogTick(30) == CJOG_ACK);
    CHECK(moveCount == 1);
}

TEST_CASE("a held start must admit a step") {
    reset();
    depth = 1;
    ringOwner = RING_JOG_STEP;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_NO_REPLY);
    depth = 0;
    admit = "busy";                                  // a sequence took the machine
    CHECK(cjogTick(10) == MSEG_NACK_BUSY);
    CHECK(cjogTick(20) == CJOG_NO_REPLY);            // dropped
    CHECK(moveCount == 0);
}

TEST_CASE("a packet the machine does not admit queues nothing") {
    reset();
    admit = "busy";                                  // legs hold the bus, ring idle
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BUSY);
    CHECK(cjogTick(10) == CJOG_NO_REPLY);
    admit = "bad_state";                             // a manual context, a latch
    CHECK(cjogPacket(XP, 64, 20) == MSEG_NACK_BAD_STATE);
    CHECK(moveCount == 0);
    CHECK(jogStops == 0);
}

TEST_CASE("a held start goes when the packets stop") {
    reset();
    depth = 1;
    ringOwner = RING_JOG_STEP;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_NO_REPLY);
    depth = 0;
    CHECK(cjogTick(CJOG_DEADMAN_MS + 1) == CJOG_NO_REPLY);
    CHECK(cjogTick(CJOG_DEADMAN_MS + 2) == CJOG_NO_REPLY);
    CHECK(moveCount == 0);
}

TEST_CASE("a held start's refusal is its reply") {
    reset();
    depth = 1;
    ringOwner = RING_JOG_STEP;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_NO_REPLY);
    depth = 0;
    at[0] = 100;                                     // no room by then
    CHECK(cjogTick(10) == MSEG_NACK_SOFT_LIMIT);
    CHECK(cjogTick(20) == CJOG_NO_REPLY);            // asked once
}

TEST_CASE("step jogs are stopped; another owner's ring is BUSY") {
    reset();
    depth = 1;
    ringOwner = RING_JOG_STEP;
    CHECK(cjogPacket(XP, 64, 0) == CJOG_NO_REPLY);
    CHECK(jogStops == 1);

    reset();
    depth = 1;
    ringOwner = RING_JOB;
    CHECK(cjogPacket(XP, 64, 0) == MSEG_NACK_BUSY);
    CHECK(cjogTick(10) == CJOG_NO_REPLY);            // nothing held
    depth = 0;
    CHECK(cjogTick(20) == CJOG_NO_REPLY);
    CHECK(jogStops == 0);
    CHECK(moveCount == 0);
}
