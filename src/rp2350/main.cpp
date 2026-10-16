// ──────────────────────────────────────────────────────────────────────────────
// Global Memory Allocation
// 
// This file allocates the physical RAM for the Cross-Core Global Variables 
// declared in shared.h. 
// 
// Note: While variables are given default zero-values here for safety, the 
// actual initialization of the machine state happens in core0.cpp during the 
// Soft Reset Sequence.
// ──────────────────────────────────────────────────────────────────────────────

#include <Arduino.h>
#include "ipc/shared_state.h"

// ─── Cross-Core Global Variables (Memory Allocation) ──────────────────────────

// Machine state, context + reason codes (see shared_state.h)
volatile uint8_t machineState   = STATE_IDLE;
volatile uint8_t machineContext = CTX_IDLE;
volatile uint8_t jobReason      = JOB_BUFFERING;
volatile uint8_t probingReason  = PROBING_CLEAR;
volatile uint8_t homingReason   = HOMING_WAIT;

// Position model (Layer 4) — counts always retained; bits say the datum is known
volatile int32_t machinePos[4]  = {0, 0, 0, 0};   // X, Y, Z, A steps
volatile uint8_t axes_homed     = 0;              // none homed until first setorigin
volatile uint8_t axes_enabled   = 0;              // derived — projectEnabled()
volatile uint16_t nodeEnabled   = 0;              // none energised until a node acks

// Inter-core request flags
volatile bool    pauseRequested = false;
volatile bool    abortRequested = false;
volatile bool    toolChangePending = false;
volatile uint8_t ringOwner      = RING_JOB;
volatile bool    estopRequested = false;
volatile bool    estopSwept     = false;
volatile float    bufferQueueS   = 1.0f;
volatile uint32_t bufferIdleMs   = 500;

// Planner motion (see shared.h) — plannerLock is claimed in setup()
planner::Planner  plannerRing;
planner::Executor plannerExec;
spin_lock_t*      plannerLock     = nullptr;
volatile bool     plannerActive   = false;
float             plannerSpm[4]   = {0, 0, 0, 0};
int64_t           plannerTurnQ32  = 0;
PlannerMesh       plannerMesh     = {nullptr, 0, 0, 0};
JobResume         jobResume       = {false, 0, 0, 0};
JobPower          jobPower        = {};
DutyStop          dutyStop        = {};
volatile bool     dutyWait        = false;
volatile uint32_t dutyEpoch       = 0;
volatile bool     toolOn          = false;

// Soft-Reset Handshake Flags
volatile bool    soft_reset_requested = true;  // Starts true so Core 1 parks on cold boot
                                                // and waits for Core 0's first wipe-and-release;
                                                // Core 0 also sets this itself at loop() entry.
volatile bool    core1_parked_for_reset      = false; // core1 parked for soft reset

// Flash-Quiesce Handshake Flags (config store) — see shared.h.
volatile bool    flash_op_requested     = false;
volatile bool    core1_parked_for_flash = false;

// Note: 
// setup() and loop() are defined in core0.cpp
// setup1() and loop1() are defined in core1.cpp
//
// The Earles F. Philhower RP2040/RP2350 core automatically links them and 
// launches Core 0 and Core 1 independently. No further code is needed here!