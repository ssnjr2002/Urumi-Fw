/**
 * The fault mask, the slots and the state they publish
 * (src/rp2350/core0/ops/machine.cpp) against faked owners.
 */

#include "doctest.h"

#include "../../src/rp2350/core0/ops/machine.cpp"

// ── fakes ───────────────────────────────────────────────────────────────────

volatile uint8_t  machineState;
volatile uint8_t  machineContext;
volatile uint8_t  ringOwner;
volatile bool     estopRequested, estopSwept, abortRequested, plannerActive;
planner::Planner  plannerRing;
spin_lock_t       lockStorage;
spin_lock_t*      plannerLock = &lockStorage;
uint8_t           homingLatched;

namespace {
bool degraded, blocking, complete = true;
bool legsLive, probeLeg;
int  dropped, aborted, probeDowns, jogStops;
bool probeRestored;

void reset() {
    degraded = blocking = legsLive = probeLeg = probeRestored = false;
    complete = true;
    dropped = aborted = probeDowns = jogStops = 0;
    homingLatched = 0;
    abortRequested = plannerActive = false;
    plannerRing.reset({0, 0});
    machineReset();
    machinePublish();
}
}  // namespace

bool busDegraded(void)        { return degraded; }
bool machineCfgBlocking()     { return blocking; }
bool slotMapComplete(void)    { return complete; }
bool runnerLive(void)         { return legsLive; }
uint8_t runnerDropAll(LegEnd*)  { dropped++; legsLive = false; return 0; }
uint8_t runnerAbortAll(LegEnd*) { aborted++; legsLive = false; return 0; }
bool probeLegInFlight(void)   { return probeLeg; }
void probeTeardown(bool restore) {
    probeDowns++;
    probeRestored = restore;
    probeLeg = false;
    contextClose(CTX_PROBE);
}
void plannerStopJog() { jogStops++; }

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

TEST_CASE("publish from the slots: context, then step, then faults") {
    reset();
    complete = false;                       // a held fault under all of them
    contextOpen(CTX_HOMING);  CHECK(machineState == STATE_HOMING);
    contextClose(CTX_PROBE);  CHECK(machineState == STATE_HOMING);   // not open
    contextClose(CTX_HOMING); CHECK(machineState == STATE_ALARM);
    contextOpen(CTX_PROBE);   CHECK(machineState == STATE_PROBING);
    contextClose(CTX_PROBE);

    legsLive = true;
    machinePublish();         CHECK(machineState == STATE_TRAVERSE);
    legsLive = false;

    ringOwner = RING_JOG_STEP;
    plannerActive = true;
    machinePublish();         CHECK(machineState == STATE_JOGGING);
    plannerActive = false;
    machinePublish();         CHECK(machineState == STATE_ALARM);

    complete = true;
    machineContext = CTX_JOB;
    machinePublish();         CHECK(machineState == STATE_JOB);
}

TEST_CASE("the estop: ESTOP until swept, then ALARM until unstop") {
    reset();
    contextOpen(CTX_HOMING);
    legsLive = true;
    estopLatch();
    CHECK(estopRequested);
    CHECK(dropped == 1);                    // dropped, not aborted: Core 1 sweeps
    CHECK(aborted == 0);
    CHECK(machineContext == CTX_IDLE);
    CHECK(machineState == STATE_ESTOP);
    CHECK(machineFaults() == F_ESTOP);
    estopSwept = true;                      // Core 1's sweep is done
    machinePublish();
    CHECK(machineState == STATE_ALARM);
    faultsClearEvents();                    // `unalarm` does not clear it
    CHECK(estopLatched());
    estopClear();
    CHECK(!estopRequested);
    CHECK(!estopSwept);
    CHECK(machineState == STATE_IDLE);
}

TEST_CASE("teardown: step before context; probe restored on a fault only") {
    reset();
    contextOpen(CTX_PROBE);
    probeLeg = true;
    faultSet(F_FACTS);
    CHECK(probeDowns == 1);
    CHECK(probeRestored);
    CHECK(machineState == STATE_ALARM);

    reset();
    contextOpen(CTX_PROBE);
    estopLatch();
    CHECK(probeDowns == 1);
    CHECK(!probeRestored);

    reset();
    legsLive = true;
    faultSet(F_HOMING);
    CHECK(aborted == 1);
    CHECK(machineState == STATE_ALARM);
}

TEST_CASE("a job closes on estop at once, on a fault once Core 1 has aborted") {
    reset();
    machineContext = CTX_JOB;
    estopLatch();
    CHECK(machineContext == CTX_IDLE);

    reset();
    machineContext = CTX_JOB;
    faultSet(F_FACTS);
    CHECK(abortRequested);
    CHECK(machineContext == CTX_JOB);       // still braking
    machinePublish();
    CHECK(machineState == STATE_JOB);
    abortRequested = false;                 // Core 1 dropped the ring
    machinePublish();
    CHECK(machineContext == CTX_IDLE);
    CHECK(machineState == STATE_ALARM);
}

TEST_CASE("a fault stops a jog; an abort with no job opens nothing") {
    reset();
    ringOwner = RING_JOG_CONT;
    plannerActive = true;
    faultSet(F_FACTS);
    CHECK(jogStops == 1);

    reset();
    jobAbort();
    abortRequested = false;
    machinePublish();
    CHECK(machineContext == CTX_IDLE);
    CHECK(machineState == STATE_IDLE);
}
