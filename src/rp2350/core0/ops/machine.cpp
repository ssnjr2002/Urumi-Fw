// machine.cpp — see machine.h.

#include "machine.h"
#include "position.h"          // homingLatched
#include "slot_map.h"
#include "bus.h"
#include "../../ipc/shared_state.h"
#include "../config/machine_cfg.h"
#include "hardware/sync.h"

static uint16_t events  = 0;
static bool     latched = false;

uint16_t machineFaults(void) {
    uint16_t f = events;
    if (busDegraded())        f |= F_BUS;
    if (machineCfgBlocking()) f |= F_CONFIG;
    if (!slotMapComplete())   f |= F_UNMAPPED;
    if (homingLatched)        f |= F_LATCHED;
    if (latched)              f |= F_ESTOP;
    return f;
}

void faultSet(uint16_t f)    { events |= f & F_EVENTS; }
void faultsClearEvents(void) { events = 0; }

void estopLatch(void) {
    latched = true;
    machineState = STATE_ESTOP;
}
bool estopLatched(void) { return latched; }
void estopClear(void)   { latched = false; }

void machineReset(void) {
    events  = 0;
    latched = false;
}

static void publishOver(bool anyState) {
    const uint16_t f = machineFaults();
    const uint32_t s = spin_lock_blocking(plannerLock);
    const uint8_t st = machineState;
    if (!anyState ? (st == STATE_IDLE || st == STATE_ALARM) : st != STATE_ESTOP)
        machineState = f ? STATE_ALARM : STATE_IDLE;
    spin_unlock(plannerLock, s);
}

void machineSettle(void)  { publishOver(true); }
void machinePublish(void) { publishOver(false); }
