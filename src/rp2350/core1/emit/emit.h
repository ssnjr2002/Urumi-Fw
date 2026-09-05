#pragma once
#include <stdint.h>
#include "../../ipc/core1_rpc.h"   // ProbeLegReq / ProbeLegOut
// emit.h — the contract every step emitter reports against.
//
// Split out of shared.h: Core 0 never sees an EmitResult. It lives here rather
// than in motion_limits.h because that file is scheduled for deletion, and
// rather than in ipc/ because it does not cross the core boundary -- it is
// shared between core1.cpp and the emitters that move into this directory.

// ─── Soft abort (§4.5) ────────────────────────────────────────────────────────
// How a segment ended. The emitter reports what it actually emitted in out[4]
// on EVERY path, including estop — the caller decides whether to keep it.
enum EmitResult : uint8_t {
    EMIT_DONE,        // ran to completion as planned; out[] == the ms deltas
    EMIT_RAMPED,      // decelerated to rest mid-flight — motion has ended
    EMIT_ESTOP,       // hard cut; position forfeited by choice, not necessity
    EMIT_SOFT_LIMIT,  // ramp overshoot crossed a bound (harness — not yet raised)
};

// ─── MicroSegment emitter ────────────────────────────────────────
// Drains the MicroSegment ring, emitting each segment at its planned interval,
// and owns every state transition that motion itself can cause: RUNNING on
// entry, and PAUSED / IDLE / ALARM on the way out. Returns when the ring is
// empty, when a ramp has brought motion to rest, or on estop.
//
// Caller must check mBufHead != mBufTail first -- an empty ring here would
// still publish the RUNNING transition.
void processMicroSegments(void);

// ─── Debug step burst ─────────────────────────────────────────────────────────
// Emits `count` raw stream bytes into one stream SLOT at debugStepSps steps/sec.
// Bypasses the MicroSegment path entirely — used to verify the Pico→node stream
// path in isolation. Only the node ENGAGE-bound to this slot moves, and it must
// also be enabled (CMD_ENABLE). Core 0 resolves the target bus node → slot (via
// the axis map) before posting the request, so here the arg is already a slot.
//
// slot is already resolved: Core 0 maps the target bus node to a stream slot via
// the axis map before posting. `steps` is signed -- the sign IS the direction.
void emitDebugSteps(uint8_t slot, uint16_t sps, int32_t steps);

// ─── Probe leg (docs/tool_probe.md §5.6, §5.7.1) ──────────────────────────────
// One leg of a tool-height probe: step Z under lockstep with the vacuum node
// that carries the bed-floor switch, and stop on the first open.
//
// LOCKSTEP is the whole design. On a poll step the emitter does not emit the
// next byte until the reply to this one has arrived, which converts a hard
// real-time problem (a reply that must land inside a step interval) into a soft
// one (a reply that must land at all). The failure mode becomes stall, not
// collision.
//
// Slots, not node ids, for the stream -- the session bound both before this ran.
// `vacNode` is carried anyway because the contact confirm (§5.9) uses the
// CRC-protected command path, which is node-addressed.
void emitProbeLeg(const ProbeLegReq* rq, ProbeLegOut* out);
