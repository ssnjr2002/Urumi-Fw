#pragma once
#ifndef SHARED_H
#define SHARED_H

#include <Arduino.h>
#include <stdint.h>
#include "common.h"

// ─── Pins ──────────────────────────────────────────────────────────────────────
#define RS485_TX_PIN  4
#define RS485_RX_PIN  5
#define RS485_EN_PIN  6

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
#define MSEG_FLAG_PATH_END  0x01  // Last segment in a path — Core 1 can signal idle
#define MSEG_FLAG_ESTOP     0x02  // Poison pill — flush and halt immediately
#define MSEG_FLAG_PAUSE     0x04  // Drain to this segment, then enter PAUSED
                                  // (host-inserted single-head tool-change marker)
#define MSEG_FLAG_WIRE_MASK 0x07  // firmware honours only these bits

struct MicroSegment {
    int32_t  dx;        // X axis steps (signed)
    int32_t  dy;        // Y axis steps (signed)
    int32_t  dz;        // Z axis steps (signed, +lift / -lower)
    int32_t  da;        // A axis steps (signed, tangential rotation)
    uint32_t interval;  // Step interval in CPU cycles (major axis timing)
    uint8_t  flags;     // MSEG_FLAG_* bitmask
    uint8_t  pad[3];    // Alignment padding — total struct size = 24 bytes
};

// ─── USB Wire Packet ──────────────────────────────────────────────────────────
// Binary packet framing for MicroSegments sent from host PC over USB CDC.
//
// Layout (26 bytes total):
//   [0]      magic  = 0xAB
//   [1..24]  MicroSegment (24 bytes, little-endian)
//   [25]     CRC8 over bytes [0..24]

#define MSEG_MAGIC       0xAB   // host production  — pre-computed step events
#define JOG_MAGIC        0xAE   // host-driven jog burst (same 26-byte layout as MSEG)
#define TILE_MAGIC       0xAD   // local production — SplineTile geometry packets
#define TOOL_MAGIC       0xAC   // local production — ToolConfig packets
#define MSEG_PACKET_SIZE 26     // magic(1) + MicroSegment(24) + CRC8(1)

// ACK/NACK responses (Pico → Host, 3 bytes each):
//   ACK:  [0xAA] [seq_lo] [seq_hi]
//   NACK: [0xBB] [reason] [0x00]
//     reason 0x01 = CRC error
//     reason 0x02 = buffer full (backpressure)
//     reason 0x03 = bad magic

#define MSEG_ACK         0xAA
#define MSEG_NACK        0xBB

// Binary status request/response (mirrors the text `getstate` command):
//   STATUS_REQ:  [0xA5]                                   (1 byte, no CRC)
//   STATUS_RSP:  [0xA6][state][enabled][homed][alarm][running][bufCount_lo][bufCount_hi][CRC8]  (9 bytes)
// bufCount is the number of MicroSegments currently queued in masterBuf
// (mBufTail - mBufHead, wrapped) — including the one Core 1 is mid-executing.
// Lets a host-side poll loop detect "buffer about to run dry" without
// guessing from wall-clock timing (see jog_blend_ui.py's blend/decel decision).
#define STATUS_REQ       0xA5
#define STATUS_RSP       0xA6
#define STATUS_RSP_SIZE  9

// ─── Config Blob Store (docs/config_storage.md) ───────────────────────────────
// Host↔Pico USB opcodes for the opaque msgpack config blob. Bit 7 set, so they
// stay disjoint from lowercase-ASCII control-plane text. The chunked SET
// transfer framing is defined with the receiver (data plane); these are the
// dispatch magics + the stored-blob size ceiling and NACK reasons.
#define CFG_SET_MAGIC    0xB0   // Host→Pico: begin config write (chunked payload)
#define CFG_GET_MAGIC    0xB1   // Host→Pico: stream back the active blob
#define CFG_MAX_BYTES    32768u // hard ceiling on a stored blob (8 flash sectors)

#define CFG_NACK_CRC       0x01 // CRC32 mismatch on the staged blob
#define CFG_NACK_TOO_BIG   0x02 // length 0 or > CFG_MAX_BYTES
#define CFG_NACK_BAD_STATE 0x03 // write rejected — machine not IDLE/ALARM
#define CFG_NACK_FLASH     0x04 // flash readback verify failed (or region too small)

// ─── Core0 → Core1 FIFO encoding ──────────────────────────────────────────────
// Normal command word : (CMD << 8) | node          — top 16 bits zero
// Debug step word      : (FIFO_STEP_DEBUG << 24) | (node << 16) | (count & 0xFFFF)
//   count is signed-magnitude: bit15 of the low word = direction (1 = negative)
#define FIFO_STEP_DEBUG  0xF0
#define STEP_DEBUG_SPS   1000   // fixed emit rate for debug stepping (steps/sec)
#define MSEG_NACK_CRC    0x01
#define MSEG_NACK_FULL   0x02
#define MSEG_NACK_MAGIC  0x03
#define MSEG_NACK_PAUSED 0x04   // job stream rejected — machine is PAUSED
#define MSEG_NACK_BAD_STATE 0x06 // stream/jog rejected — wrong machine state

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
//   ALARM   → IDLE      Core 0 `setorigin` (zeros position) or `unalarm`
//
// Enum values are the wire contract (getstate state=<n>); HOMING is reserved for
// a future auto-home cycle and never emitted in Phase 1.
enum MachineState : uint8_t {
    STATE_IDLE    = 0,
    STATE_RUNNING = 1,
    STATE_ESTOP   = 2,   // transient — Core 0 → Core 1 flush signal
    STATE_ALARM   = 3,
    STATE_PAUSED  = 4,
    STATE_HOMING  = 5,   // reserved (auto-home) — not implemented in Phase 1
};

// Reason codes (state_redesign Layer 2): metadata on WHY we are in a state, so
// no sub-states are needed. Values are the wire contract (getstate alarm=/running=).
enum AlarmReason : uint8_t {
    ALARM_NONE       = 0,
    ALARM_ESTOP      = 1,   // stop command or poison pill
    ALARM_CONFIG     = 2,   // reserved — invalid config (Phase 2)
    ALARM_SOFT_LIMIT = 3,   // reserved — position exceeded bounds (soft limits later)
    ALARM_HOMING_FAIL= 4,   // reserved — auto-home failure (future)
};

enum RunningReason : uint8_t {
    RUNNING_JOB = 0,        // streaming a job
    RUNNING_JOG = 1,        // emitting a host-driven jog burst
};

// ─── Cross-Core Global Variables (Extern Declarations) ────────────────────────

extern MicroSegment masterBuf[MASTER_BUF_SIZE];
extern volatile uint16_t mBufHead;
extern volatile uint16_t mBufTail;

extern volatile uint8_t machineState;     // one of MachineState
extern volatile uint8_t alarmReason;      // one of AlarmReason   (set before STATE_ALARM)
extern volatile uint8_t runningReason;    // one of RunningReason (meaningful while RUNNING)

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
// setorigin sets homed bits + zeros pos; enable/disable set/clear enabled bits;
// estop clears both.
extern volatile uint8_t axes_homed;
extern volatile uint8_t axes_enabled;

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
extern volatile bool    pauseRequested;
extern volatile bool    streamIsJog;

// Soft-Reset Handshake Flags
extern volatile bool    soft_reset_requested;
extern volatile bool    core1_is_parked;

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

#endif // SHARED_H