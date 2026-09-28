// bus.cpp — see bus.h.

#include "bus.h"
#include "position.h"
#include "slot_map.h"
#include "../config/machine_cfg.h"
#include "../usb_protocol.h"   // BUS_ADDR_MAX
#include "../../ipc/core1_rpc.h"
#include "../../ipc/shared_state.h"

static uint16_t s_mute     = 0;
static uint16_t s_excluded = 0;

// Every node id the config names, axes and peripherals.
static uint16_t configNodes(void) {
    if (!machineCfgValid()) return 0;
    const MachineCfg& c = machineCfg();
    uint16_t m = 0;
    auto add = [&m](const CfgNode& n) { if (n.present) m |= (1u << n.id); };
    add(c.x.node);
    add(c.y.node);
    for (uint8_t h = 0; h < c.headCount; h++) { add(c.heads[h].z.node); add(c.heads[h].a.node); }
    for (uint8_t p = 0; p < c.periphCount; p++) add(c.peripherals[p]);
    return m;
}

uint16_t busTouched(void) {
    uint16_t m = nodeEnabled;
    for (uint8_t s = 0; s < MOTION_SLOTS; s++)
        if (slotNodeAt(s) != SLOT_NONE) m |= (1u << slotNodeAt(s));
    return m & ~1u;
}

void busSweep(void) {
    uint16_t unconfirmed = 0;
    for (uint8_t n = 1; n <= BUS_ADDR_MAX; n++) {
        NodeStatus st;
        if (slotMakeSafe(n, &st) != RPC_OK || (st.flags & NODE_FLAG_ENABLED))
            unconfirmed |= (1u << n);
    }
    s_mute     = unconfirmed & (configNodes() | busTouched());
    s_excluded = 0;
    rpcSetExcluded(0);
}

bool busDegraded(void) { return (s_mute & ~s_excluded) != 0; }

uint16_t busMute(void)     { return s_mute; }
uint16_t busExcluded(void) { return s_excluded; }

bool busExclude(uint16_t ids) {
    if (ids & ~s_mute) return false;
    s_excluded |= ids;
    rpcSetExcluded(s_excluded);
    return true;
}
