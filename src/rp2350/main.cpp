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

// Machine state + reason codes (see shared.h for the transition map)
volatile uint8_t machineState   = STATE_IDLE;
volatile uint8_t alarmReason    = ALARM_NONE;
volatile uint8_t jobReason      = JOB_BUFFERING;
volatile uint8_t probingReason  = PROBING_CLEAR;
volatile uint8_t homingReason   = HOMING_WAIT;

// Position model (Layer 4) — counts always retained; bits say the datum is known
volatile int32_t machinePos[4]  = {0, 0, 0, 0};   // X, Y, Z, A steps
volatile uint8_t axes_homed     = 0;              // none homed until first setorigin
volatile uint8_t axes_enabled   = 0;              // derived — reconcileValidity()
volatile uint16_t nodeEnabled   = 0;              // none energised until a node acks

volatile int32_t resumePos[4]   = {0, 0, 0, 0};

// Inter-core request flags
volatile bool    pauseRequested = false;
volatile bool    abortRequested = false;
volatile uint8_t joggingReason  = JOGGING_NONE;

// Planner motion (see shared.h) — plannerLock is claimed in setup()
planner::Planner  plannerRing;
planner::Executor plannerExec;
spin_lock_t*      plannerLock     = nullptr;
volatile bool     plannerActive   = false;
float             plannerSpm[4]   = {0, 0, 0, 0};
int64_t           plannerTurnQ32  = 0;
PlannerMesh       plannerMesh     = {nullptr, 0, 0, 0};
volatile bool     resumeRequested = false;

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