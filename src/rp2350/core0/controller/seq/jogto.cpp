// jogto recipe — see jogto.h.

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "jogto.h"
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/bus.h"
#include "../../ops/frames.h"
#include "../../ops/position.h"
#include "../../ops/sequence.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"
#include "../../../ipc/core1_rpc.h"
#include "hardware/sync.h"

static float   targetX, targetY, feed;
static bool    lining;          // step 2
static char    refusal[40];

static bool jogtoNext(uint8_t) {
    if (lining) return false;
    lining = true;
    return lineTo(targetX, targetY, feed);
}

static const Sequence jogtoSequence = { SEQ_JOGTO, jogtoNext, nullptr };

const char* jogtoStart(float mx, float my) {
    if ((axes_homed & 0x3) != 0x3) return "not_homed";
    uint8_t zs[CFG_MAX_HEADS];
    const uint8_t nz = headZNodes(zs);
    for (uint8_t i = 0; i < nz; i++)
        if (!originValid(zs[i])) return "not_homed";

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

    // Every Z answers before anything moves.
    for (uint8_t i = 0; i < nz; i++) {
        const uint8_t  id  = zs[i];
        const uint16_t bit = 1u << id;
        if (busExcluded() & bit) {
            snprintf(refusal, sizeof refusal, "node %d excluded", id); return refusal;
        }
        if (busMute() & bit) {
            snprintf(refusal, sizeof refusal, "node %d mute", id); return refusal;
        }
        const RpcResult res = rpcNodeCmd(CMD_ENABLE, id, 0);
        if (res != RPC_OK) {
            snprintf(refusal, sizeof refusal, "node %d %s", id, rpcResultText(res));
            return refusal;
        }
    }

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
    // No Z to park: the line is the first step.
    if (nz == 0 && !jogtoNext(SEQ_END_STEP)) sequenceEnd();
    return nullptr;
}
