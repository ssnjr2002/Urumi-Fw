/**
 * The fault mask and the state it publishes (src/rp2350/core0/ops/machine.cpp)
 * against faked held conditions.
 */

#include "doctest.h"

#include "../../src/rp2350/core0/ops/machine.cpp"

// ── fakes ───────────────────────────────────────────────────────────────────

volatile uint8_t machineState;
spin_lock_t      lockStorage;
spin_lock_t*     plannerLock = &lockStorage;
uint8_t          homingLatched;

namespace {
bool degraded, blocking, complete = true;

void reset(uint8_t st = STATE_IDLE) {
    degraded = blocking = false;
    complete = true;
    homingLatched = 0;
    machineReset();
    machineState = st;
}
}  // namespace

bool busDegraded(void)        { return degraded; }
bool machineCfgBlocking()     { return blocking; }
bool slotMapComplete(void)    { return complete; }

// ── tests ───────────────────────────────────────────────────────────────────

TEST_CASE("each held bit follows its condition and publishes ALARM") {
    reset();
    CHECK(machineFaults() == 0);
    degraded = true;      CHECK(machineFaults() == F_BUS);
    blocking = true;      CHECK(machineFaults() == (F_BUS | F_CONFIG));
    complete = false;     CHECK(machineFaults() == (F_BUS | F_CONFIG | F_UNMAPPED));
    homingLatched = 0x04; CHECK(machineFaults() == F_HELD);
    machinePublish();
    CHECK(machineState == STATE_ALARM);
    degraded = blocking = false;
    complete = true;
    homingLatched = 0;
    machinePublish();
    CHECK(machineState == STATE_IDLE);
}

TEST_CASE("several faults show at once") {
    reset();
    complete = false;
    homingLatched = 0x01;
    faultSet(F_PROBE);
    CHECK(machineFaults() == (F_UNMAPPED | F_LATCHED | F_PROBE));
}

TEST_CASE("an event bit survives its held neighbours clearing") {
    reset();
    homingLatched = 0x01;
    faultSet(F_HOMING);
    machinePublish();
    CHECK(machineState == STATE_ALARM);
    homingLatched = 0;
    machinePublish();
    CHECK(machineFaults() == F_HOMING);
    CHECK(machineState == STATE_ALARM);
    faultsClearEvents();
    machinePublish();
    CHECK(machineState == STATE_IDLE);
}

TEST_CASE("faultSet takes event bits only") {
    reset();
    faultSet(F_BUS | F_ESTOP | F_FACTS);
    CHECK(machineFaults() == F_FACTS);
}

TEST_CASE("publish leaves a running state alone; settle closes it") {
    reset(STATE_HOMING);
    faultSet(F_HOMING);
    machinePublish();
    CHECK(machineState == STATE_HOMING);
    machineSettle();
    CHECK(machineState == STATE_ALARM);

    reset(STATE_JOB);
    complete = false;
    machinePublish();
    CHECK(machineState == STATE_JOB);

    reset(STATE_TRAVERSE);
    machineSettle();
    CHECK(machineState == STATE_IDLE);
}

TEST_CASE("the estop latch holds ALARM until it clears") {
    reset();
    estopLatch();
    CHECK(machineState == STATE_ESTOP);
    CHECK(machineFaults() == F_ESTOP);
    machinePublish();
    machineSettle();
    CHECK(machineState == STATE_ESTOP);   // Core 1's to leave
    machineState = STATE_ALARM;           // Core 1's sweep is done
    machinePublish();
    CHECK(machineState == STATE_ALARM);
    faultsClearEvents();                  // `unalarm` does not clear it
    CHECK(estopLatched());
    estopClear();
    machineSettle();
    CHECK(machineState == STATE_IDLE);
}
