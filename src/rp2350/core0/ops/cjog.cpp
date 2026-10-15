// cjog.cpp — see cjog.h.
//
// It moves one axis set: XY, Z or A. cjogHeld: packets for cjogDir keep
// arriving (until the stop byte, the deadman or another direction). cjogOn:
// its line may still run. cjogWant: a direction to start once the machine is
// at rest, after a change stopped the last jog.

#include "cjog.h"
#include <math.h>
#include <string.h>
#include "machine.h"           // machineAdmit
#include "position.h"          // SLOT_Z, SLOT_A
#include "frames.h"
#include "../usb_protocol.h"
#include "../config/machine_cfg.h"
#include "../planner/queue.h"
#include "../../ipc/shared_state.h"

// How far a held A jog runs: A has no soft range, and a cabled head winds.
#define CJOG_A_RUN 360.0f

static int8_t   cjogDir[4]  = {0, 0, 0, 0};
static int8_t   cjogWant[4] = {0, 0, 0, 0};
static uint8_t  cjogSpeed   = 0;
static bool     cjogHeld    = false;
static bool     cjogOn      = false;
static uint32_t cjogLastMs  = 0;

static bool cjogWanted() {
    return cjogWant[0] != 0 || cjogWant[1] != 0 || cjogWant[2] != 0 || cjogWant[3] != 0;
}

// machineAdmit(K_STEP) as a NACK reason, or 0.
static uint8_t cjogAdmit() {
    const char* why = machineAdmit(K_STEP | K_IN_TOOL);
    if (!why) return 0;
    return strcmp(why, "busy") == 0 ? MSEG_NACK_BUSY : MSEG_NACK_BAD_STATE;
}

void cjogStop(void) {
    if (cjogOn) plannerStopJog();
    cjogOn = cjogHeld = false;
    for (uint8_t k = 0; k < 4; k++) cjogWant[k] = 0;
}

// Queue the line for direction `d` (one axis set). Returns a NACK reason, or 0
// once queued.
static uint8_t cjogStart(const int8_t d[4], uint8_t speed) {
    if (!machineCfgValid()) return MSEG_NACK_BAD_STATE;
    const MachineCfg& cfg = machineCfg();
    float at[4];
    if (!plannerJogFrom(at, RING_JOG_CONT)) return MSEG_NACK_BAD_STATE;

    // How far each moving axis may go: to its soft end when homed with
    // softLimits, else maxTravel; A one CJOG_A_RUN. A diagonal stops at the
    // nearer end.
    float run = INFINITY, feed = INFINITY, step = 0;
    for (uint8_t k = 0; k < 4; k++) {
        if (d[k] == 0) continue;
        const CfgAxis* a = framesAxis(k);
        if (!a) return MSEG_NACK_BAD_STATE;
        const bool homed = axes_homed & (1u << k);
        if (!homed && !cfg.jogUnhomed) return MSEG_NACK_BAD_STATE;
        float lo, hi, room = k == SLOT_A ? CJOG_A_RUN : a->maxTravel;
        if (k != SLOT_A && homed && a->softLimits && configAxisRange(*a, &lo, &hi))
            room = d[k] > 0 ? hi - at[k] : at[k] - lo;
        run = fminf(run, room);
        float f = (homed ? a->jogFeed : a->jogFeedUnhomed) * speed / 64.0f;
        if (a->maxFeed > 0) f = fminf(f, a->maxFeed);
        feed = fminf(feed, f);
        step = fmaxf(step, 1 / a->stepsPerUnit);
    }
    if (!(run > step)) return MSEG_NACK_SOFT_LIMIT;
    if (!(feed > 0)) return MSEG_NACK_BAD_STATE;

    const PlannerQueueResult r =
        d[2] != 0 ? plannerQueueAxis(SLOT_Z, d[2] * run, feed, RING_JOG_CONT)
      : d[3] != 0 ? plannerQueueAxis(SLOT_A, d[3] * run, feed, RING_JOG_CONT)
      : plannerQueueLine(at[0] + d[0] * run, at[1] + d[1] * run, feed, RING_JOG_CONT);
    if (r == PQ_SOFT_LIMIT) return MSEG_NACK_SOFT_LIMIT;   // the mesh leaves Z no room
    if (r != PQ_OK) return MSEG_NACK_BAD_STATE;
    for (uint8_t k = 0; k < 4; k++) { cjogDir[k] = d[k]; cjogWant[k] = 0; }
    cjogSpeed = speed;
    cjogOn = cjogHeld = true;
    return 0;
}

uint8_t cjogPacket(const int8_t d[4], uint8_t speed, uint32_t now) {
    for (uint8_t k = 0; k < 4; k++)
        if (d[k] < -1 || d[k] > 1) return MSEG_NACK_BAD_STATE;
    if (speed == 0) return MSEG_NACK_BAD_STATE;
    const uint8_t sets = (d[0] != 0 || d[1] != 0) + (d[2] != 0) + (d[3] != 0);
    if (sets > 1) return MSEG_NACK_MIXED_AXES;

    cjogLastMs = now;
    if (sets == 0) { cjogStop(); return CJOG_NO_REPLY; }
    if (cjogHeld && memcmp(d, cjogDir, 4) == 0) return CJOG_NO_REPLY;   // a repeat

    // A jog's own ring (this one, still braking, or step jogs) is stopped and
    // the start held for rest; anything else must admit a step.
    const bool ring = plannerQueueDepth() != 0 || plannerActive || abortRequested;
    const bool own = cjogOn ||
        (ring && (ringOwner == RING_JOG_CONT || ringOwner == RING_JOG_STEP));
    if (!own)
        if (const uint8_t why = cjogAdmit()) return why;
    if (own || ring) {
        if (cjogOn || ringOwner == RING_JOG_STEP) plannerStopJog();
        cjogOn = cjogHeld = false;
        for (uint8_t k = 0; k < 4; k++) cjogWant[k] = d[k];
        cjogSpeed = speed;
        return CJOG_NO_REPLY;
    }
    if (const uint8_t why = cjogStart(d, speed)) return why;
    return CJOG_ACK;
}

uint8_t cjogTick(uint32_t now) {
    const bool silent = (now - cjogLastMs) > CJOG_DEADMAN_MS;
    const bool empty = !plannerActive && plannerQueueDepth() == 0;
    if (cjogOn && empty) cjogOn = false;              // ran to the end of travel
    if (cjogHeld && silent) cjogStop();
    if (!cjogWanted()) return CJOG_NO_REPLY;
    if (silent) { for (uint8_t k = 0; k < 4; k++) cjogWant[k] = 0; return CJOG_NO_REPLY; }
    if (!empty || abortRequested) return CJOG_NO_REPLY;
    int8_t d[4];
    for (uint8_t k = 0; k < 4; k++) { d[k] = cjogWant[k]; cjogWant[k] = 0; }
    if (const uint8_t why = cjogAdmit()) return why;
    if (const uint8_t why = cjogStart(d, cjogSpeed)) return why;
    return CJOG_ACK;
}
