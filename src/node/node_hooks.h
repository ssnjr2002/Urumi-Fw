// node_hooks.h — the core↔type contract, single source of truth.
//
// The node core (main.cpp, dispatch.cpp, rs485/) is type-agnostic; each node
// type (types/<x>/) defines these hooks. build_src_filter compiles exactly one
// type per binary, so exactly one definition of each is linked. The hooks are
// mandatory (non-weak): a build with no type selected fails at link, which is
// the desired guardrail.
#pragma once
#include <stdint.h>

// Identity — returns a NODE_TYPE_* constant (common.h). Answered by CMD_GET_TYPE.
uint8_t node_type(void);

// Type-specific init, called from setup() after the bus is up but before sei().
void    node_setup(void);

// Type-specific per-iteration work, called from loop() on every pass (after the
// command ring is drained). Must be non-blocking — it shares the loop with
// command dispatch. Stepper does everything in its RX ISR and leaves this empty;
// vacuum uses it to advance the SSR burst-fire / soft-start state machine.
void    node_loop(void);

// The effect of the generic CMD_ENABLE / CMD_DISABLE. The core frames the ACK;
// the type decides what "enabled" means (stepper: energize motor + accept
// stream; vacuum: run pump; …).
void    node_set_enabled(bool on);

// Type-specific command dispatch. Called by the core only after a command is
// NOT recognised as generic. Returns true if handled (reply staged in `reply`,
// `replyLen` set through the trailing CRC slot); false → unknown, silently
// dropped by the core.
bool    node_handle_command(const uint8_t* pkt, uint8_t len,
                            uint8_t* reply, uint8_t* replyLen);

// Type-specific status tail for the generic CMD_NODE_STATUS. Writes this type's
// state bytes into buf and returns the count. The core prepends a generic head
// ([node_type][flags]); this hook adds only the type's own bytes:
//   stepper: [pos int32 BE][slot]   vacuum: [servo-active bits][ssr state]
//   knife:   [osc on][blower duty]  a type with no extra state returns 0.
// buf has room for at least MAX_PACKET_LEN-5 bytes.
uint8_t node_status(uint8_t* buf);

// ─── Provided BY the core, FOR the types (opposite direction to the hooks) ────
// The one serializer for "everything this node knows about itself":
//   [node_type][flags][node_status() tail…]
// Writes it into buf and returns the length. Used by the generic CMD_NODE_STATUS
// and by any type that wants to answer a command with full state instead of a
// bare ACK — the stepper's CMD_ENGAGE and CMD_GET_POS both do, which is what
// makes an engage a single atomic observation of (bound, position, enabled)
// rather than a bind followed by a separate read that could straddle a reboot.
// One place to extend when new generic state (e.g. a session token) arrives.
uint8_t buildNodeStatus(uint8_t* buf);

// Set or clear one bit of the generic flags byte from a type. LOOP CONTEXT ONLY
// — it read-modify-writes a byte the core also touches, so an ISR caller could
// drop a bit written underneath it. A type whose flag is produced in an ISR must
// therefore latch it in a volatile of its own and mirror it here from
// node_loop(); the stepper does that with NODE_FLAG_LIMIT.
void    node_set_flag(uint8_t bit, bool on);

// Stage a reasoned refusal instead of a bare ACK/data reply: [ID][CMD_NAK]
// [len=2][cmd][reason][crc-slot]. For a type that wants the master to see WHY a
// command was rejected rather than the generic NAK_UNSUPPORTED every plain
// `return false` produces — CMD_HOME_LEG's intent check (docs/homing.md §1.4) is
// the first caller. Sets `replyLen`; the handler still returns true (a reply
// WAS staged) rather than false (nothing was staged, core NAKs generically).
void    node_reply_nak(uint8_t cmd, uint8_t reason, uint8_t* reply,
                       uint8_t* replyLen);
