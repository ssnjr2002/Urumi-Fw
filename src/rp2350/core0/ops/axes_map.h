#pragma once
#include <stdint.h>

// axes_map.h — which node each axis should be, applied as a slot map of the
// same ids (slot_map.h) once every named node is confirmed a stepper. The
// request itself lives in position.h, beside the axis views it decides.

enum AxesMapResult : uint8_t {
    AXES_OK,            // applied
    AXES_PENDING,       // an axis is pending: committed, not applied, NODE_FAULT
    AXES_NOT_STEPPER,   // refused, nothing changed (keepWrongType = false)
    AXES_ENGAGE,        // applied, a node refused to engage: NODE_FAULT
};

// Check every named node's type (CMD_NODE_STATUS, or make-safe for a node
// holding a fenced slot), then commit `ids` as the axes request and the slot
// request. Every named axis starts pending; a confirmed stepper clears it. A
// confirmed non-stepper refuses the whole map (`node <id> not_stepper`)
// unless `keepWrongType`, when it stays pending like a node that did not answer
// (`node <id> timeout`); a fenced node that does not confirm stays pending
// with `fenced …`. A pending axis's slot is parked, not engaged, the other
// slots bind as requested, and the machine is in ALARM_NODE_FAULT until
// `unalarm` re-checks.
// `keepWrongType` is for the config's default map only, which has no one to
// refuse. `*why`, when non-null, gets nullptr or the refusal (refusal.h).
AxesMapResult axesMapApply(const uint8_t* ids, bool keepWrongType,
                           const char** why = nullptr);

// Re-check the pending axes of the stored axes request, then commit it as the
// slot request as above. `unalarm`, the probe exit.
AxesMapResult axesMapRetry(const char** why = nullptr);

// True when `node` is an axis node the config marks present.
bool axisNodeInConfig(uint8_t node);
