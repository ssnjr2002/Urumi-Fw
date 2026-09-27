#pragma once
#include <stdint.h>

// axis_map.h — the slot map, the axes map applied through it, and the state
// they settle. Shared by the two commands, the probe session and the controller.
//
// Two layers (docs/engage_and_axis_map.md §5):
//   slot map   which node listens on each stream slot, any type. Its request
//              against the binding is the one thing ALARM_NODE_FAULT reads.
//   axes map   which node each axis should be (position.h's axes request). It
//              is applied as a slot map of the same ids once every named node
//              is confirmed a stepper.
//
// Applying is "deliberately dumb, not a diff": park every node that holds a
// slot, engage the requested ones, rebuild machinePos, axes_homed and
// homingLatched out of the ENGAGE acks. Correct even if a node reset in
// between, and this is the only code that binds slots.
//
// `quiet` suppresses the `ok` / `err …` line — the control plane owes exactly
// one reply per command, and a probe teardown or the boot map has none to give.

// Store `desired` (four bus ids or SLOT_NONE) as the slot request and apply it.
// Returns false if a node refused to engage: slots up to the failing one keep
// their new binding and the rest are unbound, so the request is unmet and the
// machine is in ALARM_NODE_FAULT. `failed`, when non-null, gets that node.
bool slotMapApply(const uint8_t* desired, bool quiet, uint8_t* failed = nullptr);

// True when the binding equals the slot request. True when nothing has been
// requested since the last wipe: unmapped is not a fault.
bool slotMapComplete(void);

// `unalarm`'s retry: re-apply the slot request, or, when it came from the axes
// map, re-check the pending axes first (axesMapRetry). Returns slotMapComplete().
bool slotMapRetry(void);

// Drop the slot request and the axes request. The soft-reset wipe.
void slotMapForget(void);

// Settle the state after the binding changed: an unmet request raises
// ALARM_NODE_FAULT; a met one clears ALARM_NODE_FAULT.
void slotMapGate(void);

enum AxesMapResult : uint8_t {
    AXES_OK,            // applied
    AXES_PENDING,       // an axis is pending: committed, not applied, NODE_FAULT
    AXES_NOT_STEPPER,   // refused, nothing changed (keepWrongType = false)
    AXES_ENGAGE,        // applied, a node refused to engage: NODE_FAULT
};

// Check every named node's type (CMD_NODE_STATUS), then commit `ids` as the
// axes request and the slot request. Every named axis starts pending; a
// confirmed stepper clears it. A confirmed non-stepper refuses the whole map
// (`err node <id> not_stepper`) unless `keepWrongType`, when it stays pending
// like a node that did not answer (`err node <id> timeout`). The slot request is
// applied only once nothing is pending; until then every slot holder is parked
// and the machine is in ALARM_NODE_FAULT, and `unalarm` re-checks.
// `keepWrongType` is for the boot default map only, which has no one to refuse.
AxesMapResult axesMapApply(const uint8_t* ids, bool quiet, bool keepWrongType);

// Re-check the pending axes of the stored axes request, then commit it as the
// slot request as above. `unalarm`, the probe exit.
AxesMapResult axesMapRetry(bool quiet);

// True when `node` is an axis node the config marks present.
bool axisNodeInConfig(uint8_t node);
