// jogto recipe — see jogto.h.

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "jogto.h"
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/frames.h"
#include "../../ops/position.h"
#include "../../ops/sequence.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"
#include "hardware/sync.h"

static float   targetX, targetY, feed;
static bool    lining;          // step 2
static char    refusal[40];

static bool jogtoNext(uint8_t) {
    if (lining) return false;
    lining = true;
    bool queued;
    if (const char* why = lineTo(targetX, targetY, feed, &queued)) {
        sequenceFail(why);
        return false;
    }
    return queued;
}

static const Sequence jogtoSequence = { SEQ_JOGTO, jogtoNext, nullptr };

const char* jogtoStart(float mx, float my) {
    if ((axes_homed & 0x3) != 0x3) return "not_homed";
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
    const MachineCfg& c = machineCfg();
    const float f = fminf(c.x.jogFeed, c.y.jogFeed);
    if (!(f > 0)) return "no_feed";

    if (const char* why = zsReady()) return why;

    targetX = mx;
    targetY = my;
    feed    = f;
    lining  = false;
    // The reason byte reads GOTO from the park on; the ring is idle and empty.
    const uint32_t s = spin_lock_blocking(plannerLock);
    ringOwner = RING_SEQ;
    spin_unlock(plannerLock, s);

    sequenceStart(&jogtoSequence);
    if (const char* why = parkZs()) {
        if (sequenceLive()) sequenceEnd();
        return why;
    }
    // No Z to park: the line is the first step, refused as the command.
    if (nz == 0) {
        lining = true;
        bool queued;
        const char* why = lineTo(targetX, targetY, feed, &queued);
        if (!queued) sequenceEnd();
        return why;
    }
    return nullptr;
}
