// steps.cpp — see steps.h.

#include <math.h>
#include <stdio.h>
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/bus.h"
#include "../../ops/homing.h"
#include "../../ops/position.h"
#include "../../ops/sequence.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"
#include "../../../ipc/core1_rpc.h"

uint16_t intervalUs(float feed, float spu) {
    const double us = round(1e6 / ((double)feed * (double)spu));
    return (uint16_t)(us < 1 ? 1 : us > 65535 ? 65535 : us);
}

uint32_t distSteps(double units, float spu) {
    const double s = round(units * (double)spu);
    return s < 1 ? 1 : (uint32_t)s;
}

int32_t coordSteps(const CfgAxis& a, double units) {
    const int32_t s = (int32_t)lround(units * (double)a.stepsPerUnit);
    return a.invertDir ? -s : s;
}

int32_t parkSteps(const CfgAxis& a) {
    return coordSteps(a, configParkPos(a));
}

uint8_t headZNodes(uint8_t* out) {
    if (!machineCfgValid()) return 0;
    const MachineCfg& c = machineCfg();
    uint8_t n = 0;
    for (uint8_t h = 0; h < c.headCount; h++)
        if (c.heads[h].z.node.present) out[n++] = c.heads[h].z.node.id;
    return n;
}

const char* zsReady(void) {
    static char refusal[40];
    uint8_t zs[CFG_MAX_HEADS];
    const uint8_t nz = headZNodes(zs);
    for (uint8_t i = 0; i < nz; i++)
        if (!originValid(zs[i])) return "not_homed";
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
    return nullptr;
}

const char* parkZs(void) {
    const MachineCfg& c = machineCfg();
    bool armed = false;
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgAxis& a = c.heads[h].z;
        if (!a.node.present) continue;
        const uint8_t    id = a.node.id;
        const CfgHoming& hm = a.homing;
        const char* why = homingArmPark(id, true, originTarget(id, parkSteps(a)),
                                        intervalUs(hm.startFeed, a.stepsPerUnit),
                                        intervalUs(hm.seekFeed, a.stepsPerUnit),
                                        hm.rampSteps);
        if (why) {
            if (armed) homingAbandon(id, LEGFAIL_REFUSED);
            return why;
        }
        armed = true;
    }
    return nullptr;
}

static const char* queueText(PlannerQueueResult r) {
    switch (r) {
        case PQ_NO_CONFIG:  return "unconfigured";
        case PQ_NO_LIMITS:  return "no_limits";
        case PQ_FULL:       return "full";
        case PQ_SOFT_LIMIT: return "soft_limit";
        default:            return "bad_state";
    }
}

const char* lineTo(float x, float y, float feed, bool* queued) {
    *queued = false;
    float at[4];
    if (!plannerJogFrom(at, RING_SEQ)) return "bad_state";
    if (fabsf(x - at[0]) < 1e-3f && fabsf(y - at[1]) < 1e-3f) return nullptr;
    const PlannerQueueResult r = plannerQueueLine(x, y, feed, RING_SEQ);
    if (r != PQ_OK) return queueText(r);
    sequenceRing();
    *queued = true;
    return nullptr;
}
