#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Controller — machine-level decisions made from the config, sitting on top of
// the control plane and driving it through the same entry points a host
// command uses. The config's defaultHead map is its first job; more of what
// web/src/controller does moves here over time (docs/plans/pico-config.md).
// ─────────────────────────────────────────────────────────────────────────────

// Commit the config's defaultHead axis map through axesMapApply, as a host
// `axes_map` would. Without a valid config nothing is requested and the machine
// settles IDLE, unmapped; a node that does not answer, or is not a stepper,
// leaves it in ALARM_NODE_FAULT. Needs Core 1
// running (it goes to the bus), so it runs in the boot sequence after the
// sweep; an accepted CFG_SET reaches it through a soft reset.
void controllerApplyDefaultMap();
