#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "common.h"
#include "hardware/sync.h"
#include <planner/executor.h>
#include <planner/planner.h>
#include <planner/mesh.h>

// shared_state.h — channels 2 and 3 of the core boundary.
//
//   Channel 2  state + flags   shared volatile globals, bidirectional, async
//   Channel 3  motion data     the planner ring, Core 0 -> Core 1
//
// Channel 1 (command/reply RPC) is ipc/core1_rpc.h.
//
// Core 1 cannot open a transaction on channel 1, so everything it must report
// -- leaves as a level signal Core 0 polls, or as a fact on the ring in
// ipc/core1_facts.h, which Core 0's observer pops every loop pass.
//
// Split out of shared.h, which had become the file everything included: it also
// held the USB wire contract (core0 only), the pin map and motion limits (core1
// only), and the FIFO word encoding (now ipc/core1_rpc.h).

// ─── Machine State ──────────────────────────────────────────────────────────
// What the host is told, computed by Core 0's machinePublish() from the
// estop latch, the context, the live step and the fault mask
// (core0/ops/machine.h); nothing else writes it, and no logic reads it.
//
//   estop latched           → ESTOP, ALARM once Core 1's sweep is done
//   context JOB|PROBE|HOMING → JOB, PROBING, HOMING
//   a leg step (IDLE)       → TRAVERSE
//   jog motion (IDLE)       → JOGGING
//   any fault               → ALARM
//   else                    → IDLE
//
// Enum values are the wire contract (get state=<n>). 1 and 4 (RUNNING,
// PAUSED) are retired.
enum MachineState : uint8_t {
    STATE_IDLE     = 0,
    STATE_ESTOP    = 2,   // latched, Core 1's sweep not yet done
    STATE_ALARM    = 3,
    STATE_HOMING   = 5,   // the manual homing context (core0/ops/homing.cpp)
    STATE_PROBING  = 6,   // the probe context (core0/ops/probe.cpp, docs/tool_probe.md)
    STATE_JOGGING  = 7,   // planner jog motion; never pauses
    STATE_TRAVERSE = 8,   // node legs outside a context (core0/ops/traverse.cpp)
    STATE_JOB      = 9,   // the job context, with its jobReason
};

// What is held between commands; written by Core 0 only, by the context's own
// module. JOB opens and closes under plannerLock: Core 1 runs a job's ring
// only while it is open.
enum MachineContext : uint8_t {
    CTX_IDLE   = 0,
    CTX_JOB    = 1,   // first record → `cancel`, an abort, estop or fault
    CTX_PROBE  = 2,   // `probe_map` → `probe_end`
    CTX_HOMING = 3,   // a seek, retract or sweep leg → `setorigin` / `home_end`
};

// What a job is doing (meaningful while CTX_JOB). Core 0 writes BUFFERING as it
// opens the job; Core 1 every change decided from the ring or at rest.
enum JobReason : uint8_t {
    JOB_BUFFERING = 0,   // at rest, waiting for enough queued motion
    JOB_RUNNING   = 1,
    JOB_HOLDING   = 2,   // braking for a pause or a late block
    JOB_PAUSED    = 3,   // at rest, ring dropped; a matching LAYER_HDR or `cancel`
    JOB_ABORTING  = 4,   // braking for an abort; ends IDLE
    JOB_TOOL_CHANGE = 5, // the tool-change sequence: park Zs, unlock the head's A, a line to `mount`
    JOB_TOOL_WAIT   = 6, // at rest: the operator mounts and zeroes; a header leaves
    JOB_DWELL       = 7, // at rest before a plunge, the tool off until its duty resets
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
// `get` -- that would put a bus transaction inside a read-only query, which
// query.cpp refuses to do for the same reason everywhere else.
enum ProbingReason : uint8_t {
    PROBING_LEG     = 0,   // a leg is executing; the bus is starved
    PROBING_CLEAR   = 1,   // between legs, switch closed  — tool off the surface
    PROBING_CONTACT = 2,   // between legs, switch open    — tool on the surface
};

// The homing session's phase (meaningful while STATE_HOMING). The machine stays
// in HOMING across legs, so this is what says a leg has finished.
enum HomingReason : uint8_t {
    HOMING_LEG  = 0,   // a leg is executing
    HOMING_WAIT = 1,   // between legs: next leg, setorigin or home_end
};

// Who owns the planner ring. Core 0 writes it when it restarts an empty, idle
// ring, before the first push. Values are the wire's JOGGING reason.
enum RingOwner : uint8_t {
    RING_JOB      = 0,   // the job context's records
    RING_JOG_STEP = 1,   // `jog`, `line`, `bez`
    RING_JOG_CONT = 2,   // a continuous jog, held by deadman packets
    RING_SEQ      = 3,   // a sequence's RING step (`goto`); wire JOGGING_GOTO
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern volatile uint8_t machineState;     // one of MachineState; machinePublish only
extern volatile uint8_t machineContext;   // one of MachineContext
extern volatile uint8_t jobReason;        // one of JobReason (meaningful while CTX_JOB)
extern volatile uint8_t probingReason;    // one of ProbingReason (meaningful while PROBING)
extern volatile uint8_t homingReason;     // one of HomingReason (meaningful while HOMING)
extern volatile uint8_t ringOwner;        // one of RingOwner

// The estop, Core 0 → Core 1: `stop` sets estopRequested; Core 1 drops the
// ring, sweeps the bus safe, then sets estopSwept. Every emitter stops on
// estopRequested. Both clear at `unstop` and the soft reset.
extern volatile bool    estopRequested;
extern volatile bool    estopSwept;

// Machine position in steps (X,Y,Z,A), owned and accumulated by Core 1 as it
// emits. Counts are always relatively accurate; the homed bit is what says the
// datum is trustworthy.
extern volatile int32_t machinePos[4];

// The config's job buffer thresholds (machine.buffer), published by Core 0 when
// a config loads or is adopted; Core 1 is parked then.
extern volatile float    bufferQueueS;
extern volatile uint32_t bufferIdleMs;

// Position model (state_redesign Layer 4): orthogonal per-axis physical state,
// bit0=X bit1=Y bit2=Z bit3=A. `axes_homed` replaces the old positionValid bool
// (datum known per axis); `axes_enabled` tracks which axis nodes are energised —
// a present-but-disabled axis silently drops steps, so pre-flight checks it.
// setorigin sets homed bits + zeros pos; estop clears both.
//
// `axes_enabled` is DERIVED, not commanded: projectEnabled() rebuilds it each
// Core 0 loop pass by projecting `nodeEnabled` (below) through the bound axes. No
// command handler writes it. Same shape as axes_homed ← nodeHomed and
// homingLatched ← nodeLatched (core0/ops/position.h), which is the standing rule
// here: the NODE frame is the truth, the SLOT frame is a view of it.
extern volatile uint8_t axes_homed;
extern volatile uint8_t axes_enabled;

// bit n = bus node n is energised. The node-frame truth behind `axes_enabled`.
//
// SINGLE WRITER: Core 1, and only ever as the outcome of a bus transaction it
// actually completed — an ack in rpc_server.cpp, or the safe-off sweep in
// bus/packet.cpp. Core 0 reads it and never writes it.
//
// It lives here rather than beside its siblings in core0/ops/position.cpp because it
// is the one node-frame mask with an ASYNCHRONOUS writer: Core 1 sweeps the bus
// on estop and soft reset without being asked, so Core 0 cannot own it without
// racing its own read-modify-writes (which is what it used to do — Core 0's
// `axes_enabled |= …` against Core 1 zeroing the byte mid-update). nodeHomed and
// nodeLatched stay Core-0-private precisely because nothing writes them
// unprompted.
//
// Node-framed, not slot-framed, for the same reason nodeLatched is
// (core0/ops/position.h): energisation is a fact about a motor, and it survives a
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
// Core 1 sweeps it in busStopAll().
#define BUS_ADDR_MAX 8

// Inter-core request flags: Core 0 records intent, Core 1 enacts the state
// transition.
//   pauseRequested — operator `pause`; Core 1 holds and lands JOB.PAUSED.
//   abortRequested — host `ABORT` or a jog stop; Core 1 brakes, drops the
//     ring with position intact, and clears it. Not resumable.
//   toolChangePending — a layer header wants a tool change once the job's ring
//     runs dry; Core 1 lands JOB.TOOL_CHANGE instead of BUFFERING there.
extern volatile bool    pauseRequested;
extern volatile bool    abortRequested;
extern volatile bool    toolChangePending;

// ─── Planner motion (docs/plans/pico-planner.md) ──────────────────────────────
// One lib/planner ring and its executor, shared by both cores: Core 0 pushes
// and commits (core0/planner/), Core 1 ticks (core1/emit/follower.cpp). Every
// ring edit is made holding plannerLock; planner/planner.h lists which calls.
//
// plannerActive — Core 1 owns planner motion: running, or held in
//   JOB.BUFFERING. Core 0 resets the ring and executor only while it is
//   false. Written under the lock.
// plannerSpm — steps per mm (A: per degree) for slots X, Y, Z, A, set by Core 0
//   with that reset; negative on an invertDir axis, so the planner works in
//   machine units; 0 on a slot no head binds. plannerTurnQ32 — one turn of A in
//   steps, Q32.32, signed as plannerSpm[3]: A's step target is exact however
//   many turns it has made.
// ringOwner — which kind owns the ring; set by Core 0 with that reset,
//   under the lock.
// plannerMesh — the bed mesh for this ring (core0/ops/mesh.h), latched by
//   Core 0 with that reset: Core 1 adds meshAt(*mesh, pos + tip) − ref to Z.
//   `mesh` is null while the mesh is inactive.
// plannerBlade — the mounted tool's blade offset for this ring, latched by
//   Core 0 with that reset. The planner's XY is the blade tip; the motors put
//   the knife axis bladeLead(A) ahead of it.
// jobResume — where a paused job stopped: the record and mm into its curve
//   (0 for its travel). Core 1 sets it under the lock as it lands JOB.PAUSED
//   and drops the ring; `known` is false if the ring was empty, when the job
//   resumes at the next record Core 0 expects.
// jobExec — the tag of the job block Core 1 last claimed, set under the lock
//   by Core 1 as it claims one; Core 0 clears `known` as it opens or resumes
//   a job.
extern planner::Planner  plannerRing;
extern planner::Executor plannerExec;
extern spin_lock_t*      plannerLock;
extern volatile bool     plannerActive;
extern float             plannerSpm[4];
extern int64_t           plannerTurnQ32;
struct PlannerMesh {
    const planner::Mesh* mesh;
    float tipX, tipY;   // mm, anchor to tip
    float ref;          // mm, the height at the work origin
};
extern PlannerMesh       plannerMesh;
struct PlannerBlade {
    float d;            // mm, the tip's trail behind the axis; 0 = none
    float aOffsetDeg;   // the edge's heading at machine A = 0
};
extern PlannerBlade      plannerBlade;
// The axis − tip at machine A `aDeg`: d along the edge heading.
static inline planner::Vec2 bladeLead(const PlannerBlade& b, float aDeg) {
    if (b.d == 0) return {0, 0};
    const float h = (aDeg + b.aOffsetDeg) * planner::kRadPerDeg;
    return {b.d * cosf(h), b.d * sinf(h)};
}
struct JobResume {
    bool     known;
    uint16_t layer;
    uint32_t record;
    float    s;
};
extern JobResume         jobResume;
struct JobExec {
    bool     known;
    uint16_t layer;
    uint32_t record;
};
extern JobExec           jobExec;

// Why a lift's blocks lift, a mask on each (Block::lift): Z, the A, events or
// XY it carries, and the plunge back. One lift sets every reason due at its
// spot. A START's travel is not a lift.
enum LiftReason : uint8_t {
    LIFT_END      = 1 << 0,   // after a contour's last record
    LIFT_CORNER   = 1 << 1,   // a swivel past cornerAngleDeg at a BREAK
    LIFT_DUTY     = 1 << 2,   // a duty stop planned at a BREAK
    LIFT_DEADLINE = 1 << 3,   // a duty stop where the deadline fell (plannerDutyLift)
    LIFT_UNWIND   = 1 << 4,   // A unwound by whole periods at a smooth join
    LIFT_PAUSE    = 1 << 5,
    LIFT_ABORT    = 1 << 6,
    LIFT_DRY      = 1 << 7,   // reserved: a ring run dry does not lift
};

// jobLift — Core 1's, set under the lock as it claims a job block: `last` the
// mask of the latest lift, kept past it; `count` the lifts begun; `cur` the
// claimed block's mask. Core 0 zeroes it as it opens a job.
struct JobLift {
    uint8_t  last;
    uint8_t  cur;
    uint16_t count;
};
extern JobLift           jobLift;

// Tool events (docs/plans/job-z.md, ZB): EVENT blocks Core 0 queues around a
// powered tool's plunges and lifts, run by Core 1 at rest.
enum ToolEvent : uint8_t {
    TOOL_EV_ON  = 1,   // duty wait if due (JOB.DWELL), tool on, wait settleS
    TOOL_EV_OFF = 2,
};

// The job's powered tool; node 0 for a passive one. Core 0 writes it as it
// opens a layer, under the lock. Every block in the ring is that tool's: a
// layer with another tool drains the ring first (a tool change).
// dutyEpoch: Core 0 bumps it to reset the duty counters (a job's open, another
// tool, an alarm); Core 1 resets them at its next event.
// toolOn: Core 1's, as its last switch left the tool; Core 0 reads it with the
// ring idle, to queue a TOOL_EV_OFF after a pause's or an abort's lift.
struct JobPower {
    uint8_t node;
    uint8_t cmd;       // CMD_KNIFE_OSC or CMD_LASER
    float   settleS;
    bool    hasDuty;
    float   minOnS;
    float   dwellS;
    float   maxOnS;
    float   dutyLiftS;   // a lift from the cut to clearance, s; Core 0's, per record
};
extern JobPower          jobPower;
extern volatile uint32_t dutyEpoch;
extern volatile bool     toolOn;
// dutyOnMs, toolOnAtMs: Core 1's duty counters, published for `get onms`: the
// closed on-spans since the reset, and when the tool last went on (millis).
// dutySeen: the dutyEpoch they count from; behind it, a reset is pending.
extern volatile uint32_t dutyOnMs;
extern volatile uint32_t toolOnAtMs;
extern volatile uint32_t dutySeen;

// A deadline duty lift (docs/plans/job-z.md, ZC2): Core 1, held at rest with
// its executor released, sets dutyStop and dutyWait and posts FACT_DUTY_HELD;
// it ticks no more until Core 0 has queued the lift and plunge ahead of the
// ring, committed, and cleared dutyWait. `cut` false: held outside a cut,
// Core 0 only replans.
struct DutyStop {
    planner::Pos at;
    bool     cut;
    uint16_t layer;
    uint32_t record;
    float    s;   // mm into the record's curve
};
extern DutyStop          dutyStop;
extern volatile bool     dutyWait;

// Soft-Reset Handshake Flags
extern volatile bool    soft_reset_requested;
extern volatile bool    core1_parked_for_reset;

// Flash-Quiesce Handshake Flags (config store).
// A flash erase/program stalls XIP for both cores, so Core 0 must stop Core 1
// executing from flash before touching it. This is SEPARATE from the soft-reset
// handshake: a config write must NOT wipe machine state (position/homing), so it
// cannot reuse soft_reset_requested. Core 0 sets flash_op_requested and waits for
// core1_parked_for_flash; Core 1 acks by spinning in a RAM-resident park loop
// (see core1.cpp) until the flag clears. Only asserted in IDLE/ALARM, never
// mid-motion.
extern volatile bool    flash_op_requested;
extern volatile bool    core1_parked_for_flash;


