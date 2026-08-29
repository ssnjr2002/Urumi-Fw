#pragma once
#include <stdint.h>
#include "shared_state.h"

// core1_rpc.h — channel 1 of the core boundary: command/reply RPC.
//
// Core 0 is always the initiator; Core 1 never opens a transaction, it only
// answers. ipc/core1_rpc.cpp is the client side, core1/rpc_server.cpp the
// server side.
//
// LEGACY: everything below is the hardware-FIFO word encoding this module
// replaces. It is moved here verbatim so shared.h can go, and is deleted as the
// queue_t transport lands. Nothing new should be written against it.

// ─── Core0 → Core1 FIFO encoding ──────────────────────────────────────────────
// Normal command word : (CMD << 8) | node          — top 16 bits zero
// Debug step: TWO words, pushed back to back —
//   word 0: (FIFO_STEP_DEBUG << 24) | (slot << 16) | (sps & 0xFFFF)
//           slot 0..3 (Core 0 resolves target bus node → slot via the axis map)
//   word 1: int32 step count, plain two's complement — sign IS the direction
//
// Both parameters ride the request rather than sitting in shared globals. That
// is not just tidiness: `step` is fire-and-forget (Core 0 pushes and returns
// immediately), so a second `step` issued before Core 1 picked up the first
// would have overwritten a shared rate and run burst #1 at burst #2's speed.
// Queued in the FIFO, each burst carries its own parameters. It also retires the
// old signed-magnitude packing — a plain int32 needs no sign-bit hack.
// Home: FOUR words, pushed back to back. CMD_HOME's payload is 11 bytes, which
// does not fit the normal command word's single spare byte, so it gets its own
// opcode and rides the same multi-word pattern as FIFO_STEP_DEBUG.
//   word 0: (FIFO_HOME << 24) | (dir << 16) | node
//   word 1: (start_interval_us << 16) | floor_interval_us
//   word 2: (ramp_steps << 16)                      — low half unused
//   word 3: max_steps (u32)
// Core 1 answers exactly like CMD_NODE_STATUS: a header word then the status
// payload packed 4 bytes/word, so Core 0 reuses popStatusPayload() unchanged.
// A NAK from the node (bad parameters) arrives as a zero-length payload, which
// is the same shape as a timeout — see the `home` command in control_plane.cpp.
#define FIFO_HOME        0xF1

#define FIFO_STEP_DEBUG  0xF0
#define STEP_DEBUG_SPS       1000        // default emit rate (steps/sec)
#define STEP_DEBUG_SPS_MAX  60000        // must fit the 16-bit field; also stays
                                         // under the ~92k bytes/s the bus can do
                                         // at 921.6 kbaud (one byte per step)
#define STEP_DEBUG_MAX  100000000L       // ~28 min at the max rate — a ceiling on
                                         // typos, not on anything useful
