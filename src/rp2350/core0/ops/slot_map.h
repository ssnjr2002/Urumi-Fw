#pragma once
#include <stdint.h>
#include "../../ipc/core1_rpc.h"    // RpcResult

// slot_map.h — which node listens on each stream slot, any type, and the
// ALARM_NODE_FAULT that follows when the binding does not match the request.
// It knows nothing about axes; axes_map.h applies through it.
//
// Applying is "deliberately dumb, not a diff": park every node that holds a
// slot, engage the requested ones, rebuild machinePos, axes_homed and
// homingLatched out of the ENGAGE acks (position.h). Correct even if a node
// reset in between, and this is the only code that binds slots.
//
// `quiet` suppresses the `ok` / `err …` line — the control plane owes exactly
// one reply per command, and a probe or the boot map has none to give.

// Slots are freed only by confirmation. A node that does not confirm its park
// or answer its engage fences the slot (position.h): the slot keeps the node,
// takes no engage, and satisfies a request of `-`. A request for a fenced slot
// first sends make-safe to its node; unconfirmed, the map fails with
// `err fenced <s0> <s1> <s2> <s3>` naming that node (`-` elsewhere).

// Store `desired` (four bus ids or SLOT_NONE) as the slot request and apply it.
// Returns false if a node refused to engage or a fence stayed: slots up to the
// failing one keep their new binding and the rest are unbound or fenced, so the
// request is unmet and the machine is in ALARM_NODE_FAULT. `failed`, when
// non-null, gets that node.
bool slotMapApply(const uint8_t* desired, bool quiet, uint8_t* failed = nullptr);

// The silent form, for the axes layer: store `req` as the slot request, marked
// with `fromAxes`, then apply it. Slots in the `skip` mask are parked, not
// engaged, so the request stays unmet. Settles the state. Returns SLOT_NONE, or
// the node that refused to engage with its result in `*res`; bit i of
// `*fenced` = slot i's fence could not be cleared.
uint8_t slotMapCommit(const uint8_t* req, bool fromAxes, uint8_t skip, RpcResult* res,
                      uint8_t* fenced);

// `err fenced …` for a `fenced` mask from slotMapCommit.
void slotMapPrintFenced(uint8_t fenced);

// The slot a status reply shows its node in: the stepper tail's slot, the
// probe vacuum's third tail byte, else SLOT_NONE. SLOT_NONE = released.
uint8_t nodeStatusSlot(const NodeStatus* st);

// Make node `n` safe. RPC_OK only when its reply shows it released: every slot
// it holds is then unbound. Otherwise every slot it holds is fenced. Does not
// settle the state.
RpcResult slotMakeSafe(uint8_t n, NodeStatus* st);

// Drop node `n` from the slot request (`-`). Does not settle the state.
void slotMapDrop(uint8_t n);

// True when the binding equals the slot request. True when nothing has been
// requested since the last wipe: unmapped is not a fault.
bool slotMapComplete(void);

// True when the slot request was written by the axes map, whose retry
// re-checks its pending axes first (`unalarm` picks the retry).
bool slotMapFromAxes(void);

// Re-apply the slot request as it stands. Returns slotMapComplete().
bool slotMapRetry(void);

// Drop the slot request. The soft-reset wipe.
void slotMapForget(void);

// Settle the state after the binding changed: an unmet request raises
// ALARM_NODE_FAULT; a met one clears ALARM_NODE_FAULT.
void slotMapGate(void);
