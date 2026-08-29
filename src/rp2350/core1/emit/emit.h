#pragma once
#include <stdint.h>
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

// ─── Debug step burst ─────────────────────────────────────────────────────────
// Emits `count` raw stream bytes into one stream SLOT at debugStepSps steps/sec.
// Bypasses the MicroSegment path entirely — used to verify the Pico→node stream
// path in isolation. Only the node ENGAGE-bound to this slot moves, and it must
// also be enabled (CMD_ENABLE). Core 0 resolves the target bus node → slot (via
// the axis map) before pushing the FIFO word, so here the arg is already a slot.
//
// slot is already resolved: Core 0 maps the target bus node to a stream slot via
// the axis map before posting. `steps` is signed -- the sign IS the direction.
void emitDebugSteps(uint8_t slot, uint16_t sps, int32_t steps);
