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
