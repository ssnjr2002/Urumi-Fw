// Initialisation point, mainly used to initiailse the 
// Cross-Core Global Variables before they go their separate ways

#include <Arduino.h>
#include "shared.h"

// ─── Cross-Core Global Variables (Memory Allocation) ──────────────────────────

// The Ring Buffer
MicroSegment masterBuf[MASTER_BUF_SIZE];
volatile uint16_t mBufHead = 0;
volatile uint16_t mBufTail = 0;

// Machine state + reason codes (see shared.h for the transition map)
volatile uint8_t machineState   = STATE_IDLE;
volatile uint8_t alarmReason    = ALARM_NONE;
volatile uint8_t runningReason  = RUNNING_JOB;

// Position model (Layer 4) — counts always retained; bits say the datum is known
volatile int32_t machinePos[4]  = {0, 0, 0, 0};   // X, Y, Z, A steps
volatile uint8_t axes_homed     = 0;              // none homed until first setorigin
volatile uint8_t axes_enabled   = 0;              // none energised until enable

// Paused-job context (slimmed for Phase 1 — see shared.h)
volatile bool    jobActive      = false;
volatile int32_t resumePos[4]   = {0, 0, 0, 0};

// Inter-core request flags
volatile bool    pauseRequested = false;
volatile bool    streamIsJog    = false;

// Job timing diagnostic (see shared.h) — gated behind DEBUG_TIMING
#ifdef DEBUG_TIMING
volatile uint32_t jobExpectedUs = 0;
volatile uint32_t jobMeasuredUs = 0;
volatile uint32_t jobWallUs     = 0;
#endif

// Note: 
// setup() and loop() are defined in core0.cpp
// setup1() and loop1() are defined in core1.cpp
//
// The Earles F. Philhower RP2040/RP2350 core automatically links them and 
// launches Core 0 and Core 1 independently. No further code is needed here!