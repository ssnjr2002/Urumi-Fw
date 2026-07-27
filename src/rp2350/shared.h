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

// Inter-byte timeout for a half-received fixed-26 packet. A whole packet
// arrives in microseconds over USB CDC, so a gap this long means the host
// died or desynced mid-frame — orders of magnitude below CFG_RX_TIMEOUT_MS,
// which covers a multi-kilobyte transfer.
#define FIXED26_RX_TIMEOUT_MS 50u

// ACK/NACK responses (Pico → Host, 3 bytes each):
//   ACK:  [0xAA] [expectedSeq] [0x00]   cumulative: seqs below expectedSeq accepted
//   NACK: [0xBB] [reason] [0x00]
//     reason 0x01 = CRC error
//     reason 0x02 = buffer full (backpressure)
//     reason 0x03 = bad magic

#define MSEG_ACK         0xAA
#define MSEG_NACK        0xBB

// ACKs are coalesced: because the ACK is cumulative, one frame can confirm a
// run of packets, and USB CDC charges per transaction rather than per byte.
// Pending ACKs are flushed when the input drains, when this many accumulate,
// and always before a NACK or a duplicate ACK. See docs/comms_architecture.md §4.1.
#define ACK_COALESCE_MAX 8      // ≈ half a typical host window

// Binary status request/response (mirrors the text `getstate` command).
// docs/comms_architecture.md §4.2 + §4.6.
//
//   STATUS_REQ:  [0xA5]                                    (1 byte, no CRC)
//   STATUS_RSP:  [0xA7]                                    (30 bytes)
//     [0]      magic 0xA7
//     [1]      machineState
//     [2]      axes_enabled
//     [3]      axes_homed
//     [4]      alarmReason
//     [5]      runningReason
//     [6..7]   bufCount   u16 LE   — segments queued in masterBuf
//     [8..23]  pos[4]     i32 LE   — machinePos, x/y/z/a
//     [24]     expectedSeq         — next wire seq the data plane will execute
//     [25..28] queuedUs   u32 LE   — motion time queued, microseconds
//     [29]     CRC8 over [0..28]
//
// One frame, one coherent sample. Position used to need a separate `getpos` on
// the text plane, so state and position could disagree by tens of ms; per §1
// the extra bytes are free because cost is per-transaction, not per-byte.
//
// bufCount counts segments — including the one Core 1 is mid-executing — but
// segments have wildly different durations, so queuedUs is what a jog source
// actually paces against. Whole-segment granularity: the executing segment is
// counted in full, without subtracting elapsed time. Error ≤ one segment.
//
// expectedSeq is INFORMATIONAL — for resynchronising after a timeout, abort or
// reconnect. It is not flow control; ACKs remain the only advance mechanism (D9).
//
// The magic changed 0xA6 → 0xA7 deliberately. The reader consumes fixed-length
// frames blind (D2), so a host expecting the 9-byte v1 frame must fail on an
// unknown byte (D5) rather than silently mis-parse 30 bytes as 9 and desync.
#define STATUS_REQ       0xA5
#define STATUS_RSP_V1    0xA6   // retired 9-byte frame — never emit; reserved so
                                // the value is not reused for something else
#define STATUS_RSP       0xA7
#define STATUS_RSP_SIZE  30

// Binary `seqreset` (§4.3): one byte, zeroes expectedSeq, replies ACK(0).
// The text command sits on the critical path of every stream start — the one
// text round-trip a session cannot avoid — dragging a pure data-plane session
// through the one-outstanding text plane. Replying with an ACK is exact ("I
// expect seq 0 next") and keeps the session on a single sink. The text alias
// stays for bring-up.
#define SEQRESET_MAGIC   0xA8

// Soft abort (§4.5): one byte, no reply. Core 1 ramps to rest, flushes the ring
// and lands IDLE with position intact. Confirmation arrives on the status sink
// as the state settles — like `stop`, it correlates nothing, so it needs no ACK.
#define ABORT_MAGIC      0xA9

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

// ─── Config Blob Store (docs/config_storage.md) ───────────────────────────────
// USB opcodes for the opaque msgpack config blob. Host→Pico magics have bit 7
// set, disjoint from lowercase-ASCII control-plane text. CFG_SET is a two-phase
// transfer: host sends the header, Pico replies CFG_RDY (or CFG_NACK), then host
// streams the payload; see docs/config_storage.md §5 for the full framing.
#define CFG_SET_MAGIC     0xB0  // Host→Pico: config write — header, then (on RDY) payload
#define CFG_GET_MAGIC     0xB1  // Host→Pico: request the active blob
#define CFG_RDY           0xB2  // Pico→Host: header accepted — send payload
#define CFG_ACK           0xB3  // Pico→Host: blob committed
#define CFG_NACK          0xB4  // Pico→Host: rejected — next byte is the reason
#define CFG_DATA          0xB5  // Pico→Host: CFG_GET response header

#define CFG_MAX_BYTES     32768u // hard ceiling on a stored blob (8 flash sectors)
#define CFG_RX_TIMEOUT_MS 2000u  // inter-byte timeout during a CFG_SET transfer

#define CFG_NACK_CRC       0x01 // CRC32 mismatch on the staged blob
#define CFG_NACK_TOO_BIG   0x02 // length 0 or > CFG_MAX_BYTES
#define CFG_NACK_BAD_STATE 0x03 // write rejected — machine not IDLE/ALARM
#define CFG_NACK_FLASH     0x04 // flash readback verify failed (or region too small)
#define CFG_NACK_TIMEOUT   0x05 // transfer stalled — no byte within CFG_RX_TIMEOUT_MS

// ─── Core0 → Core1 FIFO encoding ──────────────────────────────────────────────
// Normal command word : (CMD << 8) | node          — top 16 bits zero
// Debug step word      : (FIFO_STEP_DEBUG << 24) | (slot << 16) | (count & 0xFFFF)
//   slot 0..3 (Core 0 resolves the target bus node → slot via the axis map)
//   count is signed-magnitude: bit15 of the low word = direction (1 = negative)
#define FIFO_STEP_DEBUG  0xF0
#define STEP_DEBUG_SPS       1000   // default emit rate for debug stepping (steps/sec)
#define STEP_DEBUG_SPS_MAX  40000   // ceiling — one stream byte per step, and the
                                    // bus tops out near 92k bytes/s at 921.6 kbaud
// Emit rate for the NEXT debug-step burst. Core 0 writes it just before pushing
// the FIFO word (the FIFO word itself is full: tag | slot | signed count), Core 1
// reads it once at the top of the burst. Single writer, so no locking needed.
extern volatile uint32_t debugStepSps;
#define MSEG_NACK_CRC    0x01
#define MSEG_NACK_FULL   0x02
#define MSEG_NACK_MAGIC  0x03
#define MSEG_NACK_PAUSED 0x04   // job stream rejected — machine is PAUSED
#define MSEG_NACK_BAD_STATE 0x06 // stream/jog rejected — wrong machine state
// Abort is a BARRIER: everything sent before it is discarded, everything after
// waits for IDLE. Distinct from BAD_STATE so the host can treat it as "retry
// shortly" rather than surfacing an error — accepting these would mean blending
// into a deceleration and ramping back up from an arbitrary velocity, at which
// point abort stops meaning anything definite.
#define MSEG_NACK_ABORTING  0x07

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
    // Decelerating to rest after a pause/abort request. A RunningReason and not
    // a MachineState deliberately (docs/comms_architecture.md §4.5): the machine
    // IS running, so every existing IDLE/RUNNING/PAUSED gate stays correct
    // untouched, and an un-updated host reads it as plain RUNNING — which is true.
    RUNNING_ABORT_DECEL = 2,
};

// ─── Soft abort (§4.5) ────────────────────────────────────────────────────────
// How a segment ended. The emitter reports what it actually emitted in out[4]
// on EVERY path, including estop — the caller decides whether to keep it.
enum EmitResult : uint8_t {
    EMIT_DONE,        // ran to completion as planned; out[] == the ms deltas
    EMIT_RAMPED,      // decelerated to rest mid-flight — motion has ended
    EMIT_ESTOP,       // hard cut; position forfeited by choice, not necessity
    EMIT_SOFT_LIMIT,  // ramp overshoot crossed a bound (harness — not yet raised)
};

// Velocity at or below which a stop needs no ramp — start/stop speed.
#define V_REST_SPS   50.0f

// TEMPORARY — per-axis decel rate for the soft-abort ramp, steps/s².
//
// These belong in the config blob alongside the accel limits they are derived
// from, not in a header. They are #defines only because Core 1 has no
// config-read path yet — the same gap that keeps rampStepInBounds() a stub.
// Fix both together and delete this block.
//
// Seeded from web/demo/config.json as maxAccel (mm/s²) × stepsPerUnit
// (steps/mm), which is the same conversion the host planner does:
//   X  1000 × 160    = 160000
//   Y  1000 × 160    = 160000
//   A   500 ×  45.46 =  22730
// Z has NO maxAccel in that config — 150000 is a placeholder chosen to be
// unremarkable next to X/Y, not a measured limit. Treat it as unverified.
//
// Note the spread: stopping distance is v²/2a, so at 160000 steps/s² a
// 20 kHz move stops in ~1250 steps while the A axis takes ~8800. One global
// value could not have served both, which is the concrete argument for these
// being per-axis config rather than a constant.
#define DECEL_SPS2_X  160000.0f
#define DECEL_SPS2_Y  160000.0f
#define DECEL_SPS2_Z  150000.0f   // placeholder — no maxAccel in config
#define DECEL_SPS2_A   22730.0f

// A zero or negative rate makes the ramp loop non-terminating. Keep an
// equivalent runtime guard when these move into config.
static_assert(DECEL_SPS2_X > 0.0f && DECEL_SPS2_Y > 0.0f &&
              DECEL_SPS2_Z > 0.0f && DECEL_SPS2_A > 0.0f,
              "decel must be positive or the ramp never ends");

// The ramp paces the MAJOR axis — that is the axis `interval` describes, and the
// one the Bresenham accumulators are measured against — so the rate is selected
// by major-axis index, not by whichever axis is most constrained.
static inline float decelForAxis(int axis) {
    switch (axis) {
        case 0:  return DECEL_SPS2_X;
        case 1:  return DECEL_SPS2_Y;
        case 2:  return DECEL_SPS2_Z;
        default: return DECEL_SPS2_A;
    }
}

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

#endif // SHARED_H