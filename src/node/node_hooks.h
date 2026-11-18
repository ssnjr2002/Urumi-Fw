// node_hooks.h — the contract between the node core and a node type.
//
// The core (main.cpp, dispatch.cpp, rs485/) knows no type. Each type
// (types/<x>/) defines these hooks, and build_src_filter compiles one type per
// binary. The hooks have no weak defaults, so a build with no type fails at
// link. See docs/system/nodes.md §5.
#pragma once
#include <stdint.h>

// A NODE_TYPE_* constant (common.h), for CMD_GET_TYPE and the status.
uint8_t node_type(void);

// Type init, from setup() before the USART starts and before sei().
void    node_setup(void);

// Every loop() pass, before the command queue is drained. Must not block.
void    node_loop(void);

// The effect of CMD_ENABLE / CMD_DISABLE. The core sends the ACK.
void    node_set_enabled(bool on);

// The type's half of CMD_MAKE_SAFE, after node_set_enabled(false): drop the
// stream slot and park whatever disable leaves in place.
void    node_release(void);

// A command the core did not handle. Returns true with the reply staged in
// `reply` and `replyLen` set through the trailing CRC slot; false makes the
// core NAK it as unsupported.
bool    node_handle_command(const uint8_t* pkt, uint8_t len,
                            uint8_t* reply, uint8_t* replyLen);

// The type's tail of the status payload; returns its length. buf has room for
// at least MAX_PACKET_LEN-5 bytes.
uint8_t node_status(uint8_t* buf);

// ─── Provided by the core, for the types ─────────────────────────────────────

// The status payload, [node_type][flags][node_status() tail…]; returns its
// length. For a type that answers a command with full state.
uint8_t buildNodeStatus(uint8_t* buf);

// CMD_MAKE_SAFE without the reply: disable, clear ENABLED | DATUM, then
// node_release(). Also run by the bus-silence timeout. Loop context only.
void    node_make_safe(void);

// Set or clear one bit of the flags byte. Loop context only: it
// read-modify-writes a byte the core also writes. A flag produced in an ISR is
// latched in the type's own volatile and mirrored here from node_loop().
void    node_set_flag(uint8_t bit, bool on);

// True if that bit of the flags byte is set.
bool    node_flag(uint8_t bit);

// Stage a refusal with a reason, [ID][CMD_NAK][2][cmd][reason][crc-slot], and
// set `replyLen`. The handler then returns true, since a reply was staged.
void    node_reply_nak(uint8_t cmd, uint8_t reason, uint8_t* reply,
                       uint8_t* replyLen);
