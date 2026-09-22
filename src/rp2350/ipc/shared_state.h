#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "common.h"

// shared_state.h — channels 2 and 3 of the core boundary.
//
//   Channel 2  state + flags   shared volatile globals, bidirectional, async
//   Channel 3  motion data     the MicroSegment ring, Core 0 -> Core 1
//
// Channel 1 (command/reply RPC) is ipc/core1_rpc.h.
//
// Core 1 cannot open a transaction on channel 1, so everything it must report
// -- estop, soft-limit trip, position advance -- leaves as a level signal that
// Core 0 polls. That is why reconcileValidity() runs every loop pass.
//
// Split out of shared.h, which had become the file everything included: it also
// held the USB wire contract (core0 only), the pin map and motion limits (core1
// only), and the FIFO word encoding (now ipc/core1_rpc.h).

// ─── Buffer config ────────────────────────────────────────────────────────────
#define MASTER_BUF_SIZE          512
#define MASTER_BUF_LOW_WATERMARK 384

// ─── MicroSegment ─────────────────────────────────────────────────────────────
// One pre-computed step event produced by the host PC and consumed by Core 1.
// All kinematics are resolved on the PC; the Pico is a dumb step emitter.
//
// dx/dy/dz/da: signed step counts per axis for this segment (major axis = 1).
// interval:    time to wait before emitting, in RP2350 CPU cycles.
// flags:       MSEG_FLAG_* bitmask (see below).

// One byte, one namespace (see docs/wire_protocol.md). Low 3 bits are
// wire/firmware semantics; high bits (0x08 LIFT, 0x10 JOG) are host planning
// hints the firmware ignores — mask to the low 3 bits before interpreting.
#define MSEG_FLAG_NONE      0x00

// DECLARATIVE ONLY — the firmware does not act on this bit.
//
// Meaning is exactly what the name says: this is the last segment in a path.
// Hosts may set it and offline tools may read it (the planner's MICRO_PATH_END
// is the same bit with the same meaning, so a marked packet reads correctly at
// both layers). Core 1 ignores it: bit 0 is deliberately OUT of WIRE_MASK
// below, so setting it can never change machine behaviour.
//
// It is kept because the name describes something real that the wire has no
// other way to say. The firmware cannot currently distinguish "the ring went
// dry because the host is late" from "the ring went dry because the motion is
// over" — both look like an empty ring, and the first one stops an open-loop
// machine dead at speed. This bit is the natural marker for that distinction
// if it is ever wanted. (A starvation timeout is the stronger fix, since it
// also covers a host that dies mid-stream and never sends the marker — see
// docs/comms_architecture.md §4.7. Nothing here presumes which wins.)
//
// To make it live: add it to WIRE_MASK and handle it in core1.cpp. Until then
// it is a name, not a behaviour — do not read it as one.
#define MSEG_FLAG_PATH_END  0x01  // Last segment in a path (advisory; not honoured)

#define MSEG_FLAG_ESTOP     0x02  // Poison pill — flush and halt immediately
#define MSEG_FLAG_PAUSE     0x04  // Drain to this segment, then enter PAUSED
                                  // (host-inserted single-head tool-change marker)
// Host-only hint bits. The firmware never reads these; they are listed so the
// namespace stays documented in one place and nobody reuses the values.
//   0x08 LIFT, 0x10 JOG           — host planning hints (choreograph).
//   0x20 DUTY_RELEASE, 0x40 ASSERT — release / re-assert a duty-limited tool's
//                                    enable line at this segment. Always paired
//                                    with MSEG_FLAG_PAUSE, since the host can
//                                    only relay to the node once we are PAUSED.
//                                    See docs/tool_duty_limits.md.
//   0x80                          — free.

#define MSEG_FLAG_WIRE_MASK 0x06  // firmware honours only these bits — PATH_END
                                  // (0x01) is excluded on purpose, see above

struct MicroSegment {
    int32_t  dx;        // X axis steps (signed)
    int32_t  dy;        // Y axis steps (signed)
    int32_t  dz;        // Z axis steps (signed, +lift / -lower)
    int32_t  da;        // A axis steps (signed, tangential rotation)
    uint32_t interval;  // Step interval in CPU cycles (major axis timing)
    uint8_t  flags;     // MSEG_FLAG_* bitmask
    uint8_t  pad[3];    // Alignment padding — total struct size = 24 bytes
};

// Duration of a MicroSegment in microseconds: the major axis takes one step per
// `interval` cycles, so the segment lasts maxSteps × interval cycles. 64-bit
// intermediate — interval × maxSteps overflows u32 readily (a 1 s segment is
// 150e6 cycles).
static inline uint32_t microSegmentUs(int32_t dx, int32_t dy, int32_t dz,
                                      int32_t da, uint32_t interval) {
    int32_t  d[4] = { dx, dy, dz, da };
    uint32_t maxSteps = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t a = (d[i] < 0) ? (uint32_t)(-d[i]) : (uint32_t)d[i];
        if (a > maxSteps) maxSteps = a;
    }
    return (uint32_t)(((uint64_t)interval * maxSteps) / (F_CPU / 1000000u));
}

// ─── Machine State ──────────────────────────────────────────────────────────
// Single authoritative state for the controller, owned across both cores.
//   IDLE/RUNNING transition freely (Core 1, driven by the segment queue).
//   ESTOP/ALARM are sticky — only cleared by setorigin / unalarm (Core 0).
//   PAUSED suspends a job mid-stream; only resume / cancel / stop exit it.
//
// Transition map:
//   IDLE    → RUNNING   Core 1, queue non-empty
//   RUNNING → IDLE      Core 1, queue drained
//   RUNNING → PAUSED    Core 1, on MSEG_FLAG_PAUSE or pauseRequested (drain first)
//   PAUSED  → RUNNING   Core 1, jog burst arrives (runningReason = JOG)
//   PAUSED  → IDLE      Core 0 `resume` (Phase 1: host pre-positioned) or `cancel`
//   any     → ESTOP     Core 0 `stop`, or MSEG_FLAG_ESTOP poison pill
//   ESTOP   → ALARM     Core 1, after flushing the queue (position now invalid)
//   ALARM   → IDLE      Core 0 `setorigin` (sets the datum) or `unalarm`
//   IDLE    → HOMING    Core 0 `home`, once the node acks the arm
//   HOMING  → IDLE      Core 0 supervisor, node's pulser stopped as expected
//   HOMING  → ALARM     Core 0 supervisor, ALARM_HOMING_FAIL (see homing.cpp)
//
// Enum values are the wire contract (getstate state=<n>). HOMING is entered only
// by `home` and is a MOTION state: the data plane refuses MSEG and JOG in it for
// free, since both admit only IDLE/RUNNING and IDLE/PAUSED respectively.
enum MachineState : uint8_t {
    STATE_IDLE    = 0,
    STATE_RUNNING = 1,
    STATE_ESTOP   = 2,   // transient — Core 0 → Core 1 flush signal
    STATE_ALARM   = 3,
    STATE_PAUSED  = 4,
    STATE_HOMING  = 5,   // a node-run home is in progress (core0/homing.cpp)
    // A tool-height probe session is open (core0/probe.cpp, docs/tool_probe.md).
    // A SESSION state, not a motion state: it is entered by `probe_map`, spans
    // several legs with the host deciding between them, and is left only by an
    // explicit exit. Like HOMING it is neither IDLE nor RUNNING, so the data
    // plane refuses MSEG and JOG in it for free.
    STATE_PROBING = 6,
};

// Reason codes (state_redesign Layer 2): metadata on WHY we are in a state, so
// no sub-states are needed. Values are the wire contract (getstate alarm=/running=).
enum AlarmReason : uint8_t {
    ALARM_NONE       = 0,
    ALARM_ESTOP      = 1,   // stop command or poison pill
    ALARM_CONFIG     = 2,   // reserved — invalid config (Phase 2)
    ALARM_SOFT_LIMIT = 3,   // reserved — position exceeded bounds (soft limits later)
    ALARM_HOMING_FAIL= 4,   // home ended wrong: no switch found, or none cleared
    ALARM_NODE_FAULT = 5,   // reserved — a node reported or failed a check
    // A limit switch is held down. Not a failure: it is where a SEEK is
    // supposed to end, and legs 1 and 3 of a home both finish here
    // (docs/homing.md §2.6). It is an alarm because the machine genuinely
    // cannot do work in this condition — the node refuses stream steps
    // outright while its limit is latched, so a job admitted here would run
    // three axes and silently drop the fourth. ALARM already gates the data
    // plane while busGateDenies() still admits control commands, which is
    // exactly the gating a mid-home pause wants, for free.
    ALARM_LIMIT_LATCHED = 6,
    // A probe leg ended wrong (docs/tool_probe.md §5.11.1). Mirrors
    // ALARM_HOMING_FAIL and is deliberately NOT one of the reasons
    // position.cpp invalidates the origin on: most probe failures leave the Z
    // datum intact, and only the two that lost track of steps void it -- which
    // the probe supervisor does itself, per cause, rather than by reason.
    //
    // A host that does not know this value must not read it as "no alarm". The
    // web host currently degrades an unrecognised AlarmReason silently, so it
    // needs teaching alongside this.
    ALARM_PROBE_FAIL = 7,
};

enum RunningReason : uint8_t {
    RUNNING_JOB = 0,        // streaming a job
    RUNNING_JOG = 1,        // emitting a host-driven jog burst
    // Decelerating to rest after a pause/abort request. A RunningReason and not
    // a MachineState deliberately (docs/comms_architecture.md §4.5): the machine
    // IS running, so every existing IDLE/RUNNING/PAUSED gate stays correct
    // untouched, and an un-updated host reads it as plain RUNNING — which is true.
    RUNNING_ABORT_DECEL = 2,
};

// Why we are in STATE_PROBING (docs/tool_probe.md §5.2). Activity, plus the one
// phase fact the Pico owns.
//
// A session state destroys the leg-done signal that HOMING->IDLE gave the host,
// so it has to be put back: LEG -> not-LEG IS that signal. The distinction is
// mechanically real -- during a leg Core 1 holds the emit position and
// rpcServerPoll() is starved; between legs it is not -- so the reason marks
// exactly when the bus is available for supervision.
//
// CLEAR/CONTACT is not leg identity smuggled back in. The Pico never learns
// which leg of four it is running. But it does know the switch, and the switch
// partitions the legs: open means a retract is the only thing that can legally
// happen next, closed means a descent can.
//
// DERIVED, not latched as a phase flag: it is refreshed from an actual switch
// read at every leg boundary and at the exit gate. Between legs nothing moves,
// so nothing else can change it. It is deliberately NOT re-read on each
// `getstate` -- that would put a bus transaction inside a read-only query, which
// query.cpp refuses to do for the same reason everywhere else.
enum ProbingReason : uint8_t {
    PROBING_LEG     = 0,   // a leg is executing; the bus is starved
    PROBING_CLEAR   = 1,   // between legs, switch closed  — tool off the surface
    PROBING_CONTACT = 2,   // between legs, switch open    — tool on the surface
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern MicroSegment masterBuf[MASTER_BUF_SIZE];
extern volatile uint16_t mBufHead;
extern volatile uint16_t mBufTail;

// Queued motion time (§4.6), as two MONOTONIC counters rather than one shared
// total. Each has exactly one writer — Core 0 adds on enqueue, Core 1 adds on
// retire — so neither core ever read-modify-writes the other's value, the same
// single-writer discipline that makes mBufHead/mBufTail safe without a lock.
// A shared `queuedUs -= …` would be a genuine cross-core race.
//
// Read it as `queuedUsIn - queuedUsOut`, which is correct across u32 wrap
// (~71 minutes of queued motion) because unsigned subtraction wraps with it.
// On a ring flush both are resynced, since flushed segments are never retired.
extern volatile uint32_t queuedUsIn;      // Core 0 writes
extern volatile uint32_t queuedUsOut;     // Core 1 writes

static inline uint32_t queuedUs() { return queuedUsIn - queuedUsOut; }

extern volatile uint8_t machineState;     // one of MachineState
extern volatile uint8_t alarmReason;      // one of AlarmReason   (set before STATE_ALARM)
extern volatile uint8_t runningReason;    // one of RunningReason (meaningful while RUNNING)
// One of ProbingReason (meaningful while STATE_PROBING). A separate byte rather
// than a second meaning for runningReason: getstate publishes that one as
// `running=`, and two unrelated facts in one wire field is how a host ends up
// decoding a probe phase as a jog.
extern volatile uint8_t probingReason;

// Machine position in steps (X,Y,Z,A), owned and accumulated by Core 1 per
// completed segment. The consumer (Core 1) is the single source of truth so it
// stays correct regardless of whether segments came from the host or, later,
// a local on-Pico planner. Counts are always relatively accurate; the homed
// bit is what says the datum is trustworthy.
extern volatile int32_t machinePos[4];

// Position model (state_redesign Layer 4): orthogonal per-axis physical state,
// bit0=X bit1=Y bit2=Z bit3=A. `axes_homed` replaces the old positionValid bool
// (datum known per axis); `axes_enabled` tracks which axis nodes are energised —
// a present-but-disabled axis silently drops steps, so pre-flight checks it.
// setorigin sets homed bits + zeros pos; estop clears both.
//
// `axes_enabled` is DERIVED, not commanded: reconcileValidity() rebuilds it each
// Core 0 loop pass by projecting `nodeEnabled` (below) through the axis map. No
// command handler writes it. Same shape as axes_homed ← nodeHomed and
// homingLatched ← nodeLatched (core0/position.h), which is the standing rule
// here: the NODE frame is the truth, the SLOT frame is a view of it.
extern volatile uint8_t axes_homed;
extern volatile uint8_t axes_enabled;

// bit n = bus node n is energised. The node-frame truth behind `axes_enabled`.
//
// SINGLE WRITER: Core 1, and only ever as the outcome of a bus transaction it
// actually completed — an ack in rpc_server.cpp, or the safe-off sweep in
// bus/packet.cpp. Core 0 reads it and never writes it.
//
// It lives here rather than beside its siblings in core0/position.cpp because it
// is the one node-frame mask with an ASYNCHRONOUS writer: Core 1 sweeps the bus
// on estop and soft reset without being asked, so Core 0 cannot own it without
// racing its own read-modify-writes (which is what it used to do — Core 0's
// `axes_enabled |= …` against Core 1 zeroing the byte mid-update). nodeHomed and
// nodeLatched stay Core-0-private precisely because nothing writes them
// unprompted.
//
// Node-framed, not slot-framed, for the same reason nodeLatched is
// (core0/position.h): energisation is a fact about a motor, and it survives a
// rebind. Unbinding a slot does not de-energise anything.
extern volatile uint16_t nodeEnabled;

// Provisional bus-address ceiling for command relays. A real node registry
// replaces this range check when the axis-map/ENGAGE work lands
// (docs/engage_and_axis_map.md §9); until then a wrong id simply relays and
// times out.
//
// Shared between cores because the safe-off sweep must cover the WHOLE bus, not
// just the axis map: peripherals (vacuum, knife) hold no motion slot, so
// `slotNode[]` cannot reach them, and they are exactly the nodes that must not
// keep running after an estop. Core 0 range-checks operator input against this;
// Core 1 sweeps it in busDisableAll().
#define BUS_ADDR_MAX 8

// Paused-job context (state_redesign: PausedJobContext, slimmed for Phase 1).
// Captured when a job enters PAUSED. `resumePos` is the machinePos snapshot at
// the pause boundary — Phase 2's onboard auto-return target; in Phase 1 the host
// reads it via getpos and pre-positions before `resume`. `requiredAxes` is
// omitted: the Phase 1 resume gate is host-driven (PLAN_phase1_host_impl §11).
// jobActive stays true through a jog-during-pause so the burst returns to PAUSED.
extern volatile bool    jobActive;
extern volatile int32_t resumePos[4];

// Inter-core request flags (state_redesign Layer 5): Core 0 records intent, Core 1
// enacts the state transition.
//   pauseRequested — operator `pause`; Core 1 finishes the current segment,
//     snapshots resumePos, transitions to PAUSED. (MSEG_FLAG_PAUSE is detected by
//     Core 1 directly during emit; resume/cancel act from a parked buffer so
//     Core 0 transitions those itself.)
//   streamIsJog — the burst Core 0 is currently ingesting is a jog (JOG_MAGIC)
//     not a job. The discriminator is only visible at ingest, but the RUNNING
//     transition is Core 1's; Core 1 reads this to set runningReason together
//     with machineState (the cross-core stand-in for setRunning(reason)).
//   abortRequested — host `ABORT`; Core 1 ramps the CURRENT segment to rest
//     (not "finishes" it), flushes the rest of the ring, and lands IDLE with
//     position intact. Unlike pause it is not resumable. Pause now uses the same
//     ramp, so the two differ only in where Core 1 lands afterwards.
extern volatile bool    pauseRequested;
extern volatile bool    abortRequested;
extern volatile bool    streamIsJog;

// Soft-Reset Handshake Flags
extern volatile bool    soft_reset_requested;
extern volatile bool    core1_parked_for_reset;

// Flash-Quiesce Handshake Flags (config store).
// A flash erase/program stalls XIP for both cores, so Core 0 must stop Core 1
// executing from flash before touching it. This is SEPARATE from the soft-reset
// handshake: a config write must NOT wipe machine state (position/homing), so it
// cannot reuse soft_reset_requested. Core 0 sets flash_op_requested and waits for
// core1_parked_for_flash; Core 1 acks by spinning in a RAM-resident park loop
// (see core1.cpp) until the flag clears. Only asserted in IDLE/ALARM, where Core
// 1 is idle between segments — never mid-motion.
extern volatile bool    flash_op_requested;
extern volatile bool    core1_parked_for_flash;

// Job timing diagnostic (owned by Core 1, reset at each RUNNING transition).
// Gated behind DEBUG_TIMING (define it in platformio.ini build_flags to enable).
//   expected = sum of interval*maxSteps converted to us at F_CPU
//   measured = time spent inside segment emission, by the 1 MHz hardware timer
//   wall     = whole job (RUNNING -> IDLE), including buffer-empty gaps
// A ratio measured/expected != 1.0 means the cycle-counter wait loop runs at a
// different rate than F_CPU assumes; wall >> measured means streaming starvation.
#ifdef DEBUG_TIMING
extern volatile uint32_t jobExpectedUs;
extern volatile uint32_t jobMeasuredUs;
extern volatile uint32_t jobWallUs;
#endif

