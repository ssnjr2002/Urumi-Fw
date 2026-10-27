// tools — see tools.h.

#include "tools.h"
#include "frames.h"
#include "../config/machine_cfg.h"

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
