#pragma once
#include <stdint.h>
#include "../../config/config_decode.h"   // CfgAxis

// ─────────────────────────────────────────────────────────────────────────────
// The `home` recipe (docs/homing.md): homes a node list from the homing config,
// cycle by cycle. Each cycle runs its legs in parallel, in lockstep phases
// (seek, back-off, slow re-approach, pull-off; a rotary node sweeps forward
// then back in the first two), then commits one datum for every node it homed.
// Without `only`, every homeable node of an earlier cycle joins the run: a
// homed linear node parks, an unhomed one homes, a homed rotary one stays.
//
// The run holds the homing session (homingHold) from its first leg to its
// last datum; a failure is the session's (ALARM_HOMING_FAIL with homefail= and
// homenode=), and `stop` ends it as it ends any session.
// ─────────────────────────────────────────────────────────────────────────────

// Start a run for `nodes[0..n)`, each a homeable axis node of the config.
// Energises the run's nodes and arms the first phase. Returns nullptr once
// started, or the refusal text after `err ` with nothing moved. Gating the
// machine state is the caller's.
const char* homeStart(const uint8_t* nodes, uint8_t n, bool only);

// Advance the run. Call from the Core 0 loop after homingTick.
void homeTick(void);

// True from homeStart until the run commits its last cycle or is ended. The
// cycle in progress is homingHeld().
bool homeRunning(void);

// The config's axes, in order X, Y, then each head's Z and A, present nodes
// only; returns how many (at most HOME_AXES_MAX).
#define HOME_AXES_MAX (2 + 2 * CFG_MAX_HEADS)
uint8_t homeAxes(const CfgAxis** out);

// The config axis driven by `node`, or nullptr.
const CfgAxis* homeAxisFor(uint8_t node);
