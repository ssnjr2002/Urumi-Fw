#pragma once
#include <stdint.h>

// axis_map.h — the axis-map commit, shared by axis_map, the probe session and
// the controller.
//
// Split out of cmdAxisMap rather than duplicated. The probe's teardown has to
// put the committed map back (docs/tool_probe.md §5.5), and the one thing it
// must NOT do is restore from remembered state: this path is "deliberately dumb,
// not a diff" — it disengages everything and rebuilds machinePos, axes_homed and
// homingLatched out of the ENGAGE acks, so it is correct even if a node reset
// during the probe. A second binder restoring from saved fields is precisely
// where that would go wrong, and this is the only function that binds slots.
//
// `desired` is four entries: a bus id, or SLOT_NONE for an unbound slot.
// `quiet` suppresses the `ok` / `err …` line — the control plane owes exactly
// one reply per command, and when this runs as part of a probe teardown the
// probe command has already spoken (or is about to).
//
// Returns false if a node refused to engage. Slots up to the failing one keep
// their new binding and the rest are unbound, which leaves the map incomplete
// and the machine in ALARM_NODE_FAULT.
bool axisMapApply(const uint8_t* desired, bool quiet);

// True when the bound map equals the one last requested through axisMapApply
// (the controller's defaultHead map, or a host `axis_map`, which may be
// partial). False without a valid config.
bool axisMapComplete(void);

// Re-apply the last requested map. Returns axisMapComplete() afterwards.
bool axisMapRetry(void);

// True when `node` is an axis node the config marks present.
bool axisNodeInConfig(uint8_t node);

// Settle the state after the map changed: an incomplete map raises
// ALARM_NODE_FAULT (ALARM_CONFIG without a config); a complete one clears
// ALARM_NODE_FAULT. No-op while PROBING, whose exit settles the state itself.
void axisMapGate(void);
