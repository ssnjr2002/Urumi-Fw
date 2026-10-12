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
// -- estop, soft-limit trip, position advance -- leaves as a level signal that
// Core 0 polls. That is why reconcileValidity() runs every loop pass.
//
// Split out of shared.h, which had become the file everything included: it also
// held the USB wire contract (core0 only), the pin map and motion limits (core1
// only), and the FIFO word encoding (now ipc/core1_rpc.h).

// ─── Machine State ──────────────────────────────────────────────────────────
// Single authoritative state for the controller, owned across both cores.
//   ESTOP/ALARM are sticky — only cleared by setorigin / unalarm (Core 0).
//   JOB holds a job from its first record until `cancel`, an abort or estop;
//   its jobReason says what it is doing (docs/plans/motion-sessions.md).
//
// Transition map:
//   IDLE    → JOB       Core 1, planner ring (a job) non-empty: BUFFERING
//   IDLE    → JOGGING   Core 1, planner ring (a jog) non-empty
//   JOGGING → IDLE      Core 1, ring drained or the jog aborted
//   JOB     → IDLE      Core 1, abort finished; Core 0 `cancel` at rest
//   any     → ESTOP     Core 0 `stop`
//   ESTOP   → ALARM     Core 1, after dropping the ring (position now invalid)
//   ALARM   → IDLE      Core 0 `setorigin` (sets the datum) or `unalarm`
//   IDLE    → HOMING    Core 0 `home`, once the node acks the arm
//   HOMING  → IDLE      Core 0 supervisor, node's pulser stopped as expected
//   HOMING  → ALARM     Core 0 supervisor, ALARM_HOMING_FAIL (see homing.cpp)
//   IDLE    → TRAVERSE  Core 0 `leg park|span`, once the node acks the arm
//   ALARM   → TRAVERSE  Core 0 `leg unlatch`, from ALARM_LIMIT_LATCHED
//   TRAVERSE → IDLE     Core 0 supervisor, step ended (or ALARM, resumeOrHold)
//   TRAVERSE → ALARM    Core 0 supervisor, ALARM_HOMING_FAIL (see traverse.cpp)
//
// Enum values are the wire contract (get state=<n>). HOMING is entered only
// by `home` and is a MOTION state. 1 and 4 (RUNNING, PAUSED) are retired.
enum MachineState : uint8_t {
    STATE_IDLE    = 0,
    STATE_ESTOP   = 2,   // transient — Core 0 → Core 1 flush signal
    STATE_ALARM   = 3,
    STATE_HOMING  = 5,   // a node-run home is in progress (core0/ops/homing.cpp)
    // A tool-height probe session is open (core0/ops/probe.cpp, docs/tool_probe.md).
    // A SESSION state, not a motion state: it is entered by `probe_map`, spans
    // several legs with the host deciding between them, and is left only by an
    // explicit exit.
    STATE_PROBING = 6,
    // Planner jog motion (`line`, `bez`, `jog`, `jogto`; docs/plans/
    // motion-sessions.md). Never pauses: it runs, or is aborted to IDLE.
    STATE_JOGGING = 7,
    // Node legs that keep the origin (park, span, unlatch) outside a homing
    // session (core0/ops/traverse.cpp). Opened by the first leg and closed by
    // the step's end. A motion state with no reason byte.
    STATE_TRAVERSE = 8,
    // A job on the planner (BEZIER records), with its jobReason.
    STATE_JOB = 9,
};

// Reason codes (state_redesign Layer 2): metadata on WHY we are in a state, so
// no sub-states are needed. Values are the wire contract (get alarm=/reason=).
enum AlarmReason : uint8_t {
    ALARM_NONE       = 0,
    ALARM_ESTOP      = 1,   // stop command or poison pill
    // Boot found no usable config: none, an unmountable filesystem, a bad file,
    // or one that does not decode (get cfgerr=). Held until a CFG_SET
    // commit, or `uncfg` with PICO_ALLOW_UNCONFIGURED.
    ALARM_CONFIG     = 2,
    ALARM_SOFT_LIMIT = 3,   // reserved — position exceeded bounds (soft limits later)
    ALARM_HOMING_FAIL= 4,   // home ended wrong: no switch found, or none cleared
    // The slot binding does not match the slot request (slotMapComplete): a
    // node failed to engage, or an axes_map axis is pending. Held until a map
    // that is met, or `unalarm`'s retry, clears it.
    ALARM_NODE_FAULT = 5,
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
    // The boot sweep left a node mute (core0/ops/bus.h) that is not excluded.
    // The default map is not applied. Held until `bus_exclude`, or a `reset`
    // the node answers. Same host caveat as above.
    ALARM_BUS_DEGRADED = 8,
};

// What a job is doing (meaningful while STATE_JOB). Core 0 writes the entries
// it is asked for (pause); Core 1 every change decided from the ring or at rest.
enum JobReason : uint8_t {
    JOB_BUFFERING = 0,   // at rest, waiting for enough queued motion
    JOB_RUNNING   = 1,
    JOB_HOLDING   = 2,   // braking for a pause or a late block
    JOB_PAUSED    = 3,   // at rest, ring kept; `resume` or `cancel`
    JOB_ABORTING  = 4,   // braking for an abort; ends IDLE
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

// The kind of planner motion in the ring (meaningful while STATE_JOGGING).
// Core 0 writes it when it restarts an empty, idle ring, so it is settled
// before Core 1 publishes JOGGING; NONE means a job owns the ring.
enum JoggingReason : uint8_t {
    JOGGING_NONE = 0,   // a job (STATE_JOB)
    JOGGING_STEP = 1,   // `jog`, `jogto`, `line`, `bez`
    JOGGING_CONT = 2,   // a continuous jog, held by deadman packets
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern volatile uint8_t machineState;     // one of MachineState
extern volatile uint8_t alarmReason;      // one of AlarmReason   (set before STATE_ALARM)
extern volatile uint8_t jobReason;        // one of JobReason (meaningful while JOB)
extern volatile uint8_t probingReason;    // one of ProbingReason (meaningful while PROBING)
extern volatile uint8_t homingReason;     // one of HomingReason (meaningful while HOMING)
extern volatile uint8_t joggingReason;    // one of JoggingReason; see its comment

// Machine position in steps (X,Y,Z,A), owned and accumulated by Core 1 as it
// emits. Counts are always relatively accurate; the homed bit is what says the
// datum is trustworthy.
extern volatile int32_t machinePos[4];

// Position model (state_redesign Layer 4): orthogonal per-axis physical state,
// bit0=X bit1=Y bit2=Z bit3=A. `axes_homed` replaces the old positionValid bool
// (datum known per axis); `axes_enabled` tracks which axis nodes are energised —
// a present-but-disabled axis silently drops steps, so pre-flight checks it.
// setorigin sets homed bits + zeros pos; estop clears both.
//
// `axes_enabled` is DERIVED, not commanded: reconcileValidity() rebuilds it each
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

// machinePos where a job paused; `resume` refuses if XY moved since.
extern volatile int32_t resumePos[4];

// Inter-core request flags: Core 0 records intent, Core 1 enacts the state
// transition.
//   pauseRequested — operator `pause`; Core 1 holds and lands JOB.PAUSED.
//   abortRequested — host `ABORT`; Core 1 brakes, drops the ring, and lands
//     IDLE with position intact. Not resumable.
extern volatile bool    pauseRequested;
extern volatile bool    abortRequested;

// ─── Planner motion (docs/plans/pico-planner.md) ──────────────────────────────
// One lib/planner ring and its executor, shared by both cores: Core 0 pushes
// and commits (core0/planner/), Core 1 ticks (core1/emit/follower.cpp). Every
// ring edit is made holding plannerLock; planner/planner.h lists which calls.
//
// plannerActive — Core 1 owns planner motion: running, or held in JOB.PAUSED
//   or JOB.BUFFERING. Core 0 resets the ring and executor only while it is
//   false. Written under the lock.
// plannerSpm — steps per mm (A: per degree) for slots X, Y, Z, A, set by Core 0
//   with that reset; negative on an invertDir axis, so the planner works in
//   machine units; 0 on a slot no head binds. plannerTurnQ32 — one turn of A in
//   steps, Q32.32, signed as plannerSpm[3]: A's step target is exact however
//   many turns it has made.
// joggingReason — which kind owns the ring; set by Core 0 with that reset,
//   under the lock.
// plannerMesh — the bed mesh for this ring (core0/ops/mesh.h), latched by
//   Core 0 with that reset: Core 1 adds meshAt(*mesh, pos + tip) − ref to Z.
//   `mesh` is null while the mesh is inactive.
// resumeRequested — `resume` of a paused job; Core 1 replans from where it
//   stopped and returns to BUFFERING. Set and cleared under the lock.
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
extern volatile bool     resumeRequested;

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


