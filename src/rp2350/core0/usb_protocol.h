#pragma once
#include <Arduino.h>
#include <stdint.h>
#include "../ipc/shared_state.h"   // MicroSegment (MSEG packets carry one)

// usb_protocol.h — the USB CDC wire contract between the host PC and Core 0.
//
// Split out of shared.h: Core 1 never sees a USB packet. Its only appearance in
// core1.cpp was a comment.
//
// THIS FILE IS MIRRORED BY web/src/wire/format/constants.ts.
// The two must agree byte for byte. Change one, change the other.

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
