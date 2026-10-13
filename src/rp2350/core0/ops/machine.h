#pragma once

// machine.h — the fault mask, the estop latch, and the state they publish.

#include <stdint.h>

// machineFaults() bits; the values are the wire contract (STATUS_RSP, get
// faults=). Held bits are recomputed on every read from the conditions behind
// them and clear when the cause goes. Event bits are set by whatever detected
// them and cleared by `unalarm`.
enum MachineFault : uint16_t {
    F_BUS      = 0x0001,   // held: a mute node is not excluded (busDegraded)
    F_CONFIG   = 0x0002,   // held: no usable config (machineCfgBlocking)
    F_UNMAPPED = 0x0004,   // held: the slot request is unmet (slotMapComplete)
    F_LATCHED  = 0x0008,   // held: an axis stands on its switch (homingLatched)
    F_HOMING   = 0x0010,   // event: a homing or traverse leg failed
    F_PROBE    = 0x0020,   // event: a probe leg failed
    F_FACTS    = 0x0040,   // event: a Core 1 fact was dropped
    F_ESTOP    = 0x8000,   // the estop latch: not a fault, cleared by `unstop`
};
#define F_HELD   (F_BUS | F_CONFIG | F_UNMAPPED | F_LATCHED)
#define F_EVENTS (F_HOMING | F_PROBE | F_FACTS)

uint16_t machineFaults(void);
void     faultSet(uint16_t f);          // event bits only
void     faultsClearEvents(void);

// `stop` latches and signals Core 1 (STATE_ESTOP); Core 1 sweeps and publishes
// ALARM. `unstop` clears it.
void estopLatch(void);
bool estopLatched(void);
void estopClear(void);

// Soft reset: no event bits, no latch.
void machineReset(void);

// ALARM while any bit is set, else IDLE. machineSettle() writes it from any
// state, for a Core 0 state that closes (HOMING, TRAVERSE, PROBING);
// machinePublish() only over IDLE or ALARM, once per loop pass and after
// anything that changes a held bit. Both under plannerLock: Core 1 leaves IDLE
// for motion under it.
void machineSettle(void);
void machinePublish(void);
