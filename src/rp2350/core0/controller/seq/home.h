#pragma once
#include <stdint.h>
#include "../../config/config_decode.h"   // CfgAxis

// ─────────────────────────────────────────────────────────────────────────────
// The `home` recipe (docs/homing.md): homes a node list from the homing config,
// cycle by cycle. In a cycle each node runs its own chain of legs (seek,
// back-off, slow re-approach, pull-off; a rotary node sweeps forward then
// back), each armed as the one before it ends; once every node's chain is
// done, the cycle commits one datum for every node it homed.
// Without `only`, every homeable node of an earlier cycle joins the run: a
// homed linear node parks, an unhomed one homes, a homed rotary one stays.
//
// The run is the `home` sequence (ops/sequence.h), one step per cycle, each
// node a thread of it (ops/runner.h), in the context it started in; the
// cycle in progress is homingHeld(). A failure is F_HOMING with homefail= and
// homenode=; `stop` ends it as any sequence.
// ─────────────────────────────────────────────────────────────────────────────

// Start a run for `nodes[0..n)`, each a homeable axis node of the config.
// Energises the run's nodes and arms each one's first leg. Returns nullptr once
// started, or the refusal text after `err ` with nothing moved. Gating the
// machine state is the caller's.
const char* homeStart(const uint8_t* nodes, uint8_t n, bool only);

// The `recovery` sequence (unalarm, F_LATCHED): step 1 takes every latched
// node off its switch in parallel, unlatching one whose origin is valid (it
// stays) and retracting one with none; step 2 parks the nodes that kept their
// origin. Returns as homeStart; nullptr with nothing to do.
const char* recoveryStart(void);

// The config's axes, in order X, Y, then each head's Z and A, present nodes
// only; returns how many (at most HOME_AXES_MAX).
#define HOME_AXES_MAX (2 + 2 * CFG_MAX_HEADS)
uint8_t homeAxes(const CfgAxis** out);

// The config axis driven by `node`, or nullptr.
const CfgAxis* homeAxisFor(uint8_t node);
