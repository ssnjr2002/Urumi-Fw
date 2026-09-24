#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// Controller — machine-level decisions made from the config, sitting on top of
// the control plane and driving it through the same entry points a host
// command uses. The config's defaultHead map is its first job; more of what
// web/src/controller does moves here over time (docs/plans/pico-config.md).
// ─────────────────────────────────────────────────────────────────────────────

// Commit the config's defaultHead axis map through axisMapApply, as a host
// `axis_map` would. Without a valid config the machine is put in ALARM_CONFIG;
// a node that does not answer leaves it in ALARM_NODE_FAULT. Needs Core 1
// running (it goes to the bus), so it runs after a soft reset has released
// Core 1, and after an accepted CFG_SET.
void controllerApplyDefaultMap();
