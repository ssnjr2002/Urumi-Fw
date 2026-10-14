// steps.cpp — see steps.h.

#include <math.h>
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/homing.h"
#include "../../ops/position.h"
#include "../../ops/sequence.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"

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

bool lineTo(float x, float y, float feed) {
    float at[4];
    if (!plannerJogFrom(at, RING_SEQ)) return false;
    if (fabsf(x - at[0]) < 1e-3f && fabsf(y - at[1]) < 1e-3f) return false;
    if (plannerQueueLine(x, y, feed, RING_SEQ) != PQ_OK) return false;
    sequenceRing();
    return true;
}
