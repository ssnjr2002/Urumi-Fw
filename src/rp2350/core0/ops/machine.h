#pragma once

// machine.h — the fault mask, the estop latch, the context and step slots,
// and the state they publish.

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

// The live step, read from its owners: node legs (the runner, a probe leg) or
// planner motion (the ring, or Core 1 running it).
enum MachineStep : uint8_t {
    STEP_NONE = 0,
    STEP_LEGS = 1,
    STEP_RING = 2,
};

uint16_t machineFaults(void);
// Event bits only; an event fault tears down what runs (machineTeardown).
void     faultSet(uint16_t f);
void     faultsClearEvents(void);

// `stop` latches, signals Core 1 (estopRequested) and tears down; Core 1
// sweeps and sets estopSwept. `unstop` clears both.
void estopLatch(void);
bool estopLatched(void);
void estopClear(void);

// Open or close a context (MachineContext) and publish. A close names the
// context it closes and is a no-op if another is open. Not for CTX_JOB, which
// opens and closes under plannerLock (planner/queue.cpp, `cancel`).
void contextOpen(uint8_t ctx);
void contextClose(uint8_t ctx);

uint8_t machineStep(void);

// Abort a job's motion (the host's ABORT): Core 1 brakes and drops the ring;
// the job's context closes once it has.
void jobAbort(void);

// Step, then context: legs dropped (estop: Core 1 is sweeping) or aborted,
// a job's ring aborted, the context closed.
void machineTeardown(bool estop);

// Soft reset: no event bits, no latch, no context; ALARM until the boot
// sequence publishes.
void machineReset(void);

// Writes machineState from the latch, the slots and the faults (see
// ipc/shared_state.h), under plannerLock. Last in each loop pass and after
// every command; also closes a job whose abort Core 1 has finished.
void machinePublish(void);
