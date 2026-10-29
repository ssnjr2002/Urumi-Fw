#pragma once

// machine.h — the fault mask, the estop latch, the context and step levels,
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
    F_SEQUENCE = 0x0080,   // event: a sequence was refused after its ok (sequenceFail)
    F_TOOL     = 0x0100,   // event: a tool's power switch failed (a tool event)
    F_ESTOP    = 0x8000,   // the estop latch: not a fault, cleared by `unstop`
};
#define F_HELD   (F_BUS | F_CONFIG | F_UNMAPPED | F_LATCHED)
#define F_EVENTS (F_HOMING | F_PROBE | F_FACTS | F_SEQUENCE | F_TOOL)

// The live step, read from its owners: node legs (the runner, a probe leg),
// planner motion (the ring, or Core 1 running it) or the sequence's wait.
enum MachineStep : uint8_t {
    STEP_NONE = 0,
    STEP_LEGS = 1,
    STEP_RING = 2,
    STEP_WAIT = 3,
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
// context it closes and is a no-op if another is open; a close aborts a live
// step first. Not for CTX_JOB, which
// opens and closes under plannerLock (planner/queue.cpp, `cancel`).
void contextOpen(uint8_t ctx);
void contextClose(uint8_t ctx);

uint8_t machineStep(void);

// Abort a job's motion (the host's ABORT): Core 1 brakes and drops the ring;
// the job's context closes once it has.
void jobAbort(void);

// Step, sequence, then context: legs dropped (estop: Core 1 is sweeping) or
// aborted, the sequence aborted, a job's ring aborted, the context closed.
void machineTeardown(bool estop);

// Soft reset: no event bits, no latch, no context; ALARM until the boot
// sequence publishes.
void machineReset(void);

// Writes machineState from the latch, the levels and the faults (see
// ipc/shared_state.h), under plannerLock. Last in each loop pass and after
// every command; also closes a job whose abort Core 1 has finished.
void machinePublish(void);

// The state publish would write now. Core 0 logic asks this (or the levels),
// never machineState.
uint8_t machineView(void);

// Nothing moves and the bus is free: estop swept or not latched, no step, no
// sequence, and no context but a paused job.
bool machineAtRest(void);

// A step or a sequence is live: legs, the ring (a paused job's excepted), or
// a sequence between steps.
bool machineMoving(void);

// As machineAtRest, and also a homing context between legs: the bus queries.
bool machineBusFree(void);

// ── command kinds ────────────────────────────────────────────────────────────
// A column of the command tables (control_plane.cpp); machineAdmit applies the
// kind's rule before the handler runs. Argument-dependent checks stay in the
// handlers.
enum CmdKind : uint8_t {
    K_ANY      = 0,   // always: stop, and settings with no state rule
    K_READ     = 1,   // always; a bus read checks machineBusFree itself
    K_WRITE    = 2,   // at rest (machineAtRest)
    K_STEP     = 3,   // a primitive that takes time: no estop, no step or sequence, no job
    K_SEQUENCE = 4,   // IDLE context, no step, no fault but F_LATCHED
    K_CONTEXT  = 5,   // opens or closes a context, by the handler's rules; no estop
    K_CLEAR    = 6,   // unalarm, unstop: no step, no sequence, no context
};
#define K_IN_TOOL   0x10   // also admitted in JOB.TOOL_WAIT: the operator's touch-off
#define K_IN_PROBE  0x40   // also admitted inside the probe context
#define K_IN_HOMING 0x80   // also admitted inside the homing context
#define K_BARE_READ 0x20   // the bare form (no arguments) is a read
#define K_KIND(k)   ((k) & 0x0F)

// nullptr, or the refusal: "busy" when a step or sequence is what refuses,
// else "bad_state". Inside a manual context only K_ANY, K_READ and the rows
// flagged for it are admitted.
const char* machineAdmit(uint8_t kind);
