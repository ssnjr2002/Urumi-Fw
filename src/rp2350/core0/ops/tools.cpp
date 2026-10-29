// tools — see tools.h.

#include <math.h>
#include "tools.h"
#include "frames.h"
#include "machine.h"
#include "position.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"

static uint8_t mounted[CFG_MAX_HEADS];

uint8_t toolsMounted(uint8_t head) {
    return head < CFG_MAX_HEADS ? mounted[head] : 0;
}

const char* toolsMount(uint8_t head, uint8_t id) {
    const MachineCfg& c = machineCfg();
    if (head >= c.headCount) return "no_head";
    if (id != 0 && !configTool(c, id)) return "no_tool";
    if (mounted[head] != id) framesUnsetWorkZ(head);
    mounted[head] = id;
    return nullptr;
}

void toolsReset(void) {
    for (uint8_t h = 0; h < CFG_MAX_HEADS; h++) mounted[h] = 0;
}

uint8_t toolsUnlocked(void) {
    if (!machineCfgValid() || machineMoving()) return 0;
    const uint8_t ctx = machineContext;
    if (!(ctx == CTX_IDLE || (ctx == CTX_JOB && jobReason == JOB_TOOL_WAIT))) return 0;
    const MachineCfg& c = machineCfg();
    uint8_t mask = 0;
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgAxis& a = c.heads[h].a;
        int32_t at;
        if (!(c.heads[h].unlockRange > 0) || !nodeMachineSteps(a.node.id, &at)) continue;
        if (fabs(at / (double)a.stepsPerUnit) <= c.heads[h].unlockRange) mask |= 1u << h;
    }
    return mask;
}
