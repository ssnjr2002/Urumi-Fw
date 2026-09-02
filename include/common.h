#pragma once
#ifndef COMMON_H
#define COMMON_H

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          921600
#define RESPONSE_TIMEOUT_MS    20

// ─── Broadcast address ────────────────────────────────────────────────────────
// A command frame addressed here is acted on by EVERY node and answered by NONE.
// The no-reply part is structural, not per-command: N nodes replying at once is a
// bus collision, so the node suppresses TX whenever it was addressed by wildcard.
//
// 0xFF rather than 0x00 because a broadcast must be asked for, never arrived at.
// Zero is what a memset, a zeroed packet buffer or an uninitialised node variable
// produces, so a software bug would broadcast by accident; all-ones has to be
// written deliberately. (Idle RS485 floats high, so line noise reads as 0xFF —
// but a stray address byte still needs a valid cmd, length and CRC8 behind it.)
// It also matches the house sentinel already in use: slot 0xFF = disengaged.
#define BUS_ADDR_BROADCAST 0xFF
// ─── Node Commands ────────────────────────────────────────────────────────────────
// Generic (0x01–0x1F): every node type honours these; handled by the node core.
#define CMD_PING     0x01
#define CMD_PONG     0x02
#define CMD_ENABLE   0x04   // effect delegated per type (motor energize / pump on …)
#define CMD_DISABLE  0x05
#define CMD_GET_TYPE 0x06   // reply payload: [NODE_TYPE_*]

// ─── CMD_NAK — the node refused ──────────────────────────────────────────────
// REPLY-ONLY: the master never sends this opcode, and a node that receives it
// treats it as unaddressed rather than answering (dispatch.cpp).
//
//     reply: [ID][CMD_NAK][len=2][orig_cmd][NAK_*][crc]
//
// `orig_cmd` is carried because the reply opcode is no longer the request's, so
// it is the only thing tying the refusal back to what was asked.
//
// Why it exists: an unhandled command used to be dropped exactly like a bad-CRC
// frame, so the master timed out. "Refused" and "absent" then produced the same
// observation, which is why control_plane could only ever print nak_or_timeout.
// One RESPONSE_TIMEOUT_MS is also spent per refusal, waiting for an answer that
// was never coming.
//
// A NAK costs a node nothing to send and the master nothing to ignore, so an
// OLD node — one flashed before this opcode existed — still simply times out.
// That is the intended migration: the two firmwares are distinguishable on the
// wire by exactly this, and neither confuses the other.
#define CMD_NAK      0x07
#define NAK_UNSUPPORTED 0x01  // this node does not implement that opcode
#define NAK_BAD_TOKEN   0x02  // session token mismatch (plan section 8.2)
#define NAK_BAD_ARG     0x03  // opcode known, payload rejected
#define NAK_INTENT_MISMATCH 0x04  // CMD_HOME only: declared intent disagreed
                                  // with the node's own pin read — see below.
#define NAK_BUSY        0x05  // opcode known, payload fine, node already doing
                              // it. Distinct from BAD_ARG because retrying the
                              // IDENTICAL frame later is the correct response;
                              // a BAD_ARG frame is wrong however long you wait.
// Type-specific (0x20+): only one type is compiled per node, so values may
// overlap between types. Stepper:
// Node status flags (the [flags] byte of the status payload below).
#define NODE_FLAG_ENABLED 0x01  // energised now (CMD_ENABLE / CMD_DISABLE)
#define NODE_FLAG_DATUM   0x02  // CONTINUITY WITNESS — see CMD_DATUM_SET
// The witness answers one question the master cannot answer for itself: "has this
// node been energised and counting, without interruption, since I datumed it?"
// Dead reckoning is only sound while that holds. It is deliberately NOT the same
// thing as NODE_FLAG_ENABLED: a node that was disabled and re-enabled is enabled
// again but its datum is gone, because a de-energised motor can be back-driven
// with no change to its step counter.
//
// Only CMD_DATUM_SET sets it. The node NEVER sets it on its own — in particular
// CMD_ENABLE must not, or a reset followed by a re-enable would silently re-arm
// a witness for a datum that no longer exists. The node only ever CLEARS it: at
// boot (RAM init) and on CMD_DISABLE. So a brownout, a watchdog reset or a local
// de-energise all invalidate it without the master needing to observe the event.
#define CMD_DATUM_SET 0x23  // no payload; sets NODE_FLAG_DATUM, replies status payload

// NODE_FLAG_LIMIT — this node is refusing stream steps because its limit switch
// is asserted, or was asserted long enough to latch (see docs/homing.md §1.1).
// Reserved for EVERY node type and board, not only the ones with a switch wired:
// the flags byte has one meaning across the bus, so the master can decode a
// status reply without first knowing which board answered. A node with no switch
// simply never sets it — same contract as NODE_FLAG_DATUM on a vacuum node.
#define NODE_FLAG_LIMIT   0x04

// NODE_FLAG_HOMING — the local step pulser is running (CMD_HOME). Set when the
// command is accepted, cleared when the pulser stops for any reason. The master
// learns a home finished by polling this off; a node never announces it.
//
// LIMIT and HOMING together are the whole terminal-state report, and they read
// OPPOSITELY for the two kinds of move — after a seek, LIMIT set means found and
// clear means the budget ran out; after a retract it is the other way round. The
// node does not know which kind it ran (see docs/homing.md 1.2) and does not need
// to: the master sent the move, so the master does the interpreting.
#define NODE_FLAG_HOMING  0x08

// Status payload — ONE shape, from one serializer on the node (buildNodeStatus):
//     [node_type][flags][type-specific tail…]        flags: NODE_FLAG_*
//     stepper tail: [pos int32 BE][slot]             slot 0xFF = disengaged
// CMD_NODE_STATUS, CMD_GET_POS and the CMD_ENGAGE ack all reply with it, so the
// host has one parser and there is one place to extend. Notably the ENGAGE ack
// makes a bind a single atomic observation of (bound, position, enabled): a
// follow-up read could straddle a node reboot and describe a slot the node no
// longer holds. See docs/node_session_and_datum.md.
#define CMD_GET_POS  0x03   // stepper: reply = status payload (above)
#define CMD_ENGAGE   0x20   // stepper: payload [slot] 0..3, 0xFF = disengage; ack = status payload
#define CMD_LASER    0x21   // stepper (-DNODE_HAS_LASER only): payload [state 0/1]; NAK elsewhere
#define CMD_NODE_STATUS 0x22 // any type; no payload; reply = status payload (above)

// CMD_HOME — run the node's own step pulser. Only nodes with a limit switch
// wired accept it; every other stepper NAKs. See docs/homing.md 1.4.
//
//   payload (11 bytes, big-endian, matching the status tail's convention):
//     [0]    dir/intent     bit0 = wire dir bit: which way THIS move goes
//                           bit1 = intent: 0 = host expects a seek,
//                                          1 = host expects a retract
//     [1..2] start_interval microseconds, pull-in rate
//     [3..4] floor_interval microseconds, cruise rate
//     [5..6] ramp_steps     steps from start to floor; 0 = no ramp
//     [7..10] max_steps     runaway budget
//   ack: the status payload, sampled after arming (same shape as CMD_ENGAGE).
//
// There is NO SEPARATE seek/retract command or node-retained mode. The node
// still samples its limit pin when the command is accepted, and that single
// read is still what actually PICKS the mode: pin clear runs a seek (until the
// switch asserts), pin asserted runs a retract (switch ignored, budget run out
// in full). Deciding from the pin rather than from retained state is what lets
// a node that booted with its axis already parked on the switch retract
// correctly on the FIRST command — that property is unchanged.
//
// The intent bit does not steer that decision. It is a second, independent
// opinion the host attaches to let the node CATCH a disagreement it would
// otherwise execute silently. Without it, a host that thinks it is arming a
// seek (large runaway budget, meant to be cut short by the switch) but finds
// the pin already asserted -- stale prior state, a bounced or mis-wired
// switch, a leg that did not clear it as expected -- gets a retract instead:
// same huge budget, but a retract IGNORES the switch and runs it to
// completion. That is not a wrong-direction nudge, it is the full seek-sized
// runaway distance with nothing left to stop it. Comparing pin-derived mode
// against declared intent and NAKing on mismatch closes that hole for the cost
// of one bit and one comparison; nothing is stored past the single command.
//
// Intervals are MICROSECONDS, not timer ticks: the node converts on receipt, so
// the 20MHz/24MHz difference between board families never reaches the master or
// the config schema.
//
// ROTARY (-DHAS_HALL_INDEX) reuses this command unchanged. A rotary axis has no
// limit pin, so there is no mode to pick and the intent bit is IGNORED rather
// than repurposed: there is nothing for the node to disagree with, and giving
// the bit a second meaning on some boards is how a payload stops being one
// payload. Everything else — direction, both intervals, ramp, budget — means
// exactly what it means for a linear leg.
//
// What differs is the REPLY, not the request. A switch edge IS the position, so
// a linear seek reports its answer as the position it stopped at, already in the
// tail. A dip's centre is only knowable after passing it, so a rotary sweep runs
// THROUGH the feature and reports a separate index position that is not where
// the axis stopped. See the status tail below.
#define CMD_HOME     0x24
#define CMD_HOME_PAYLOAD_LEN 11

// ─── ROTARY_IDX_* — how a rotary index sweep ended ──────────────────────────
// Named for the operation, not for homing in general: these describe one
// technique (run through the magnet, buffer it, reduce it) and say nothing about
// a limit-switch leg. The supervisor's own verdict on any home, by whatever
// technique, is HOMEFAIL_* in rp2350/core0/homing.h — do not confuse the two.
//
// Reported in the stepper status tail on HAS_HALL_INDEX builds. `index` is
// meaningful ONLY for ROTARY_IDX_OK; every other value says why there is no
// answer, which a bare "did not work" could not.
//
// ONLY ROTARY_IDX_OK IS A PASS. Everything below it is a refusal, because a
// datum is either trustworthy or it is not — there is no degraded mode where
// a wrong index is better than none. They differ in what an operator should DO, not in
// whether the home succeeded, and that is why they stay distinct rather than
// collapsing to one failure code.
#define ROTARY_IDX_NONE       0  // no sweep has completed since reset. Seen AFTER a
                                 // home, it means resolve never ran — a fault in the
                                 // node, not in the mechanism.
#define ROTARY_IDX_OK         1  // index found, and proven periodic by the crossings
#define ROTARY_IDX_NOTFOUND   2  // budget ran out before the required number of
                                 // COMPLETE crossings. `crossings` disambiguates: 0
                                 // means the magnet was never seen at all (no sensor,
                                 // no magnet, or no rotation), and a short count means
                                 // the budget was simply too small.
#define ROTARY_IDX_DEGENERATE 3  // autoconvolution peak <= 0: the window held no
                                 // usable feature (sensor dead, magnet missing)
#define ROTARY_IDX_OVERFLOW   4  // dip wider than the capture buffer even at the
                                 // derived decimation — a real shape change, not a
                                 // near miss
#define ROTARY_IDX_SLIP       5  // the index was found, but the intervals between
                                 // consecutive crossings disagree by more than a
                                 // fraction of themselves: the axis slipped, stalled,
                                 // or the sensor caught something that is not
                                 // once-per-revolution. A datum measured across a slip
                                 // is wrong BY the slip. Only detectable because the
                                 // sweep crosses the index more than twice.

// ─── HOMING_KIND_* — which terminator this stepper actually has ──────────────
// Sent in the stepper status tail by EVERY stepper node, including ones with no
// homing at all. It exists because capability was previously inferred from the
// PAYLOAD LENGTH, which conflates two different things: what the board can do,
// and how old its firmware is. Length-inference also breaks silently the first
// time a field is appended to the linear tail — the lengths collide and a linear
// node decodes as rotary. Declaring the kind removes the guess.
#define HOMING_KIND_NONE  0  // no switch, no index: this node NAKs CMD_HOME
#define HOMING_KIND_LIMIT 1  // limit switch on the terminator pin (linear)
#define HOMING_KIND_INDEX 2  // Hall index, analog dip (rotary)
// Vacuum:
#define CMD_SERVO_SET 0x10  // payload: [idx(0=all,1..N)][angle(0..180)]; ACK echoes cmd
// Host-side on/off shorthand: the Pico expands "on" to this angle before it hits
// the RS485 wire (see core1.cpp); "off" is 0. The node itself takes a raw angle.
#define SERVO_ON_ANGLE  180
#define CMD_SSR_SET   0x11  // payload: [state(0=off, 1=on w/ soft-start)]; ACK echoes cmd
#define CMD_SWITCH_GET 0x14 // no payload; reply payload: [level] (raw PA3 digitalRead)
// Knife (oscillating drag knife):
#define CMD_KNIFE_OSC    0x12  // payload: [state(0=off, 1=on)]; ACK echoes cmd
#define CMD_KNIFE_BLOWER 0x13  // payload: [duty(0..100 %)]; ACK echoes cmd

// ─── Broadcast allowlist ──────────────────────────────────────────────────────
// Deny by default: a command is broadcastable only if it is named here. A command
// that becomes broadcastable because nobody thought about it is exactly the
// failure worth designing out, so the opt-in is explicit and lives in one place
// shared by both sides — the master refuses to send it, the node refuses to act.
//
// To qualify, a command must be (a) idempotent, since it cannot be retried
// per-node, and (b) useful without an answer, since none comes back.
//
// Only generic commands (0x01–0x1F) are eligible. Type-specific opcodes (0x20+)
// deliberately overlap between node types, so one broadcast value would mean
// different things to different nodes — unaddressable by construction.
static inline bool cmdAllowsBroadcast(uint8_t cmd) {
    // CMD_DISABLE = "park yourself", each type's own safe state. The estop path
    // broadcasts it so every node starts stopping in parallel, then confirms
    // serially per node. See src/rp2350/core1/core1.cpp.
    //
    // CMD_ENABLE is the symmetric arm-everything verb. Note the asymmetry in what
    // the master may CONCLUDE from each: an unacknowledged disable can only leave
    // it believing less is energised than really is (safe), while an unacknow-
    // ledged enable must never be taken as proof anything armed. Core 0 encodes
    // that — see bus_enable in control_plane.cpp.
    return cmd == CMD_DISABLE || cmd == CMD_ENABLE;
}

// ─── Node types (CMD_GET_TYPE) ───────────────────────────────────────────────
// Canonical registry, mirrored on the host (web/src/config NodeType).
#define NODE_TYPE_STEPPER  0x01
#define NODE_TYPE_VACUUM   0x02
#define NODE_TYPE_KNIFE_OSC 0x03  // oscillating (drag/tangential) knife controller

// CRC-8, reflected, poly 0x8C (industrial variant). Precomputed 256-entry
// lookup table — one byte-indexed step per input byte instead of 8 bit-shifts.
// Table generated from the bitwise algorithm and verified equivalent for all
// inputs. init = 0x00, no final xor.
//
// On AVR nodes the table lives in flash via PROGMEM to save
// 256 B of the 3 KB SRAM; read with pgm_read_byte. On the RP2350 it's a plain
// const array read directly.
#ifdef __AVR__
#include <avr/pgmspace.h>
static const uint8_t crc8_table[256] PROGMEM = {
#else
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
static const uint8_t crc8_table[256] = {
#endif
    0x00, 0x5E, 0xBC, 0xE2, 0x61, 0x3F, 0xDD, 0x83, 0xC2, 0x9C, 0x7E, 0x20, 0xA3, 0xFD, 0x1F, 0x41,
    0x9D, 0xC3, 0x21, 0x7F, 0xFC, 0xA2, 0x40, 0x1E, 0x5F, 0x01, 0xE3, 0xBD, 0x3E, 0x60, 0x82, 0xDC,
    0x23, 0x7D, 0x9F, 0xC1, 0x42, 0x1C, 0xFE, 0xA0, 0xE1, 0xBF, 0x5D, 0x03, 0x80, 0xDE, 0x3C, 0x62,
    0xBE, 0xE0, 0x02, 0x5C, 0xDF, 0x81, 0x63, 0x3D, 0x7C, 0x22, 0xC0, 0x9E, 0x1D, 0x43, 0xA1, 0xFF,
    0x46, 0x18, 0xFA, 0xA4, 0x27, 0x79, 0x9B, 0xC5, 0x84, 0xDA, 0x38, 0x66, 0xE5, 0xBB, 0x59, 0x07,
    0xDB, 0x85, 0x67, 0x39, 0xBA, 0xE4, 0x06, 0x58, 0x19, 0x47, 0xA5, 0xFB, 0x78, 0x26, 0xC4, 0x9A,
    0x65, 0x3B, 0xD9, 0x87, 0x04, 0x5A, 0xB8, 0xE6, 0xA7, 0xF9, 0x1B, 0x45, 0xC6, 0x98, 0x7A, 0x24,
    0xF8, 0xA6, 0x44, 0x1A, 0x99, 0xC7, 0x25, 0x7B, 0x3A, 0x64, 0x86, 0xD8, 0x5B, 0x05, 0xE7, 0xB9,
    0x8C, 0xD2, 0x30, 0x6E, 0xED, 0xB3, 0x51, 0x0F, 0x4E, 0x10, 0xF2, 0xAC, 0x2F, 0x71, 0x93, 0xCD,
    0x11, 0x4F, 0xAD, 0xF3, 0x70, 0x2E, 0xCC, 0x92, 0xD3, 0x8D, 0x6F, 0x31, 0xB2, 0xEC, 0x0E, 0x50,
    0xAF, 0xF1, 0x13, 0x4D, 0xCE, 0x90, 0x72, 0x2C, 0x6D, 0x33, 0xD1, 0x8F, 0x0C, 0x52, 0xB0, 0xEE,
    0x32, 0x6C, 0x8E, 0xD0, 0x53, 0x0D, 0xEF, 0xB1, 0xF0, 0xAE, 0x4C, 0x12, 0x91, 0xCF, 0x2D, 0x73,
    0xCA, 0x94, 0x76, 0x28, 0xAB, 0xF5, 0x17, 0x49, 0x08, 0x56, 0xB4, 0xEA, 0x69, 0x37, 0xD5, 0x8B,
    0x57, 0x09, 0xEB, 0xB5, 0x36, 0x68, 0x8A, 0xD4, 0x95, 0xCB, 0x29, 0x77, 0xF4, 0xAA, 0x48, 0x16,
    0xE9, 0xB7, 0x55, 0x0B, 0x88, 0xD6, 0x34, 0x6A, 0x2B, 0x75, 0x97, 0xC9, 0x4A, 0x14, 0xF6, 0xA8,
    0x74, 0x2A, 0xC8, 0x96, 0x15, 0x4B, 0xA9, 0xF7, 0xB6, 0xE8, 0x0A, 0x54, 0xD7, 0x89, 0x6B, 0x35,
};

static inline uint8_t crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    while (len--)
        crc = pgm_read_byte(&crc8_table[crc ^ *data++]);
    return crc;
}

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — stronger collision
// resistance for the config-blob correctness gate. Same value is stored in the
// flash header, returned by CMD_GET_CONFIG, and compared in the Phase 2 MCFG
// handshake (docs/wire_protocol.md). Bitwise (no table) — config writes are rare.
static inline uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (uint8_t k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

#endif