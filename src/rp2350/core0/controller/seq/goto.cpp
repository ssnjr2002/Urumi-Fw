// goto recipes — see goto.h.

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "goto.h"
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/frames.h"
#include "../../ops/position.h"
#include "../../ops/sequence.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"
#include "hardware/sync.h"

static float   targetX, targetY, feed;
static bool    lining;          // the line is queued, or there is none to come
static bool    lock;            // A rides the line to unlock
static char    refusal[40];

static bool gotoNext(uint8_t) {
    if (lining) return false;
    lining = true;
    bool queued;
    if (const char* why = lineTo(targetX, targetY, feed, &queued, lock)) {
        sequenceFail(why);
        return false;
    }
    return queued;
}

static const Sequence gotoSequence = { SEQ_GOTO, gotoNext, nullptr };

// Open the sequence with the ring owned by it, so the reason byte reads GOTO
// from the first step on; the ring is idle and empty.
static void open(bool line) {
    lining = !line;
    const uint32_t s = spin_lock_blocking(plannerLock);
    ringOwner = RING_SEQ;
    spin_unlock(plannerLock, s);
    sequenceStart(&gotoSequence);
}

// Run legs as the first step; nullptr, or the refusal with the sequence ended.
static const char* legs(const char* (*arm)(void)) {
    if (const char* why = arm()) {
        if (sequenceLive()) sequenceEnd();
        return why;
    }
    return nullptr;
}

const char* gotoLine(float mx, float my, float f, bool aLock) {
    if ((axes_homed & 0x3) != 0x3) return "not_homed";
    if (aLock) if (const char* why = aLockReady()) return why;
    uint8_t zs[CFG_MAX_HEADS];
    const uint8_t nz = headZNodes(zs);

    float at[4];
    if (!plannerJogFrom(at, RING_SEQ)) return "bad_state";
    const float to[2] = {mx, my};
    for (uint8_t k = 0; k < 2; k++) {
        float left;
        if (const char* why = framesCheckMove(k, at[k], to[k], &left)) {
            snprintf(refusal, sizeof refusal, "%s %.2f", why, left); return refusal;
        }
    }
    if (const char* why = zsReady()) return why;

    targetX = mx;
    targetY = my;
    feed    = f;
    lock    = aLock;
    open(true);
    if (nz) return legs(parkZs);
    // No Z to park: the line is the first step, refused as the command.
    lining = true;
    bool queued;
    const char* why = lineTo(targetX, targetY, feed, &queued, lock);
    if (!queued) sequenceEnd();
    return why;
}

const char* gotoAxis(uint8_t k, float to, float f) {
    if (!(axes_homed & (1u << k))) return k == SLOT_A ? "a_not_homed" : "not_homed";
    float at[4];
    if (!plannerJogFrom(at, RING_SEQ)) return "bad_state";
    float left;
    if (const char* why = framesCheckMove(k, at[k], to, &left)) {
        snprintf(refusal, sizeof refusal, "%s %.2f", why, left); return refusal;
    }
    // A swings a blade's axis about the tip: the tip inside its range keeps it in.
    if (k == SLOT_A) if (const char* why = framesCheckXY(at[0], at[1])) return why;
    if (fabsf(to - at[k]) < 1e-3f) return nullptr;

    open(false);
    const PlannerQueueResult r = plannerQueueAxis(k, to - at[k], f, RING_SEQ);
    if (r != PQ_OK) {
        sequenceEnd();
        return queueText(r);
    }
    sequenceRing();
    return nullptr;
}

const char* gotoUp(void) {
    if (const char* why = zsReady()) return why;
    uint8_t zs[CFG_MAX_HEADS];
    if (!headZNodes(zs)) return nullptr;
    open(false);
    return legs(parkZs);
}

const char* gotoUnlock(void) {
    if (const char* why = asReady()) return why;
    uint8_t as[CFG_MAX_HEADS];
    if (!headANodes(as)) return nullptr;
    open(false);
    return legs(lockAs);
}
