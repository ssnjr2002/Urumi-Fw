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
// The run is the `home` sequence (ops/sequence.h), one phase per step, in the
// context it started in; the cycle in progress is homingHeld(). A failure is
// F_HOMING with homefail= and homenode=; `stop` ends it as any sequence.
// ─────────────────────────────────────────────────────────────────────────────

// Start a run for `nodes[0..n)`, each a homeable axis node of the config.
// Energises the run's nodes and arms the first phase. Returns nullptr once
// started, or the refusal text after `err ` with nothing moved. Gating the
// machine state is the caller's.
const char* homeStart(const uint8_t* nodes, uint8_t n, bool only);

// The config's axes, in order X, Y, then each head's Z and A, present nodes
// only; returns how many (at most HOME_AXES_MAX).
#define HOME_AXES_MAX (2 + 2 * CFG_MAX_HEADS)
uint8_t homeAxes(const CfgAxis** out);

// The config axis driven by `node`, or nullptr.
const CfgAxis* homeAxisFor(uint8_t node);
