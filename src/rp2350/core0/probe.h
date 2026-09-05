#pragma once
#include <stdint.h>

// probe.h — Core 0's tool-height probe session (docs/tool_probe.md §5).
//
// A SESSION, not a move. `probe_map` opens it, any number of `probe_leg`s run
// inside it, and an explicit exit closes it — because the last leg cannot know
// it is the last: the host owns the sequence and the Pico runs one leg at a
// time, without ever learning which leg of four it is.
//
// Why a session at all, rather than a single "probe" command: the bed-floor
// switch is wired to the VACUUM node, so a probe needs the vacuum bound into a
// motion slot for the whole of it. That binding is not a machine anyone can cut
// with, which is exactly why entering it is explicit and leaving it restores.
//
// This file owns the WAITING and the JUDGING; the stepping is Core 1's
// (core1/emit/probe_leg.cpp), and the same argument homing.h makes applies
// verbatim — a handler that blocked until the leg finished would hold
// `getstate` and `stop` shut for the whole descent, on the one command that is
// driving a tool into a bed.

// Open a session: save the committed axis map, disengage everything, verify
// both nodes' types out of the disengage acks, then bind Z and the vacuum.
//
// A FULL ALTERNATIVE BINDING, not an overlay on the committed map. Two commands
// writing one slot table is the "updated one frame, forgot the other" class that
// cmd/axis.cpp says cost a 1000-line file once already.
//
// Refuses if any node fails to ack the disengage, and that is the safety
// property rather than a nicety: a poll is requested by setting the VACUUM
// slot's step bit, so a stepper still engaged in that slot would take one step
// per poll — a phantom axis tracking Z's entire descent. Verified teardown makes
// that impossible by construction.
//
// Prints exactly one reply line on every path.
bool probeBegin(uint8_t zNode, uint8_t vacNode);

// Arm one leg. Returns once the leg is POSTED, not once it has run; the result
// arrives through probeTick(). `intent` is the host's prediction of the switch
// state at the start of this leg — checked against a real read, not obeyed.
bool probeArmLeg(uint8_t dir, uint16_t startUs, uint16_t ceilUs,
                 uint16_t rampSteps, uint8_t pollDiv, uint32_t maxSteps,
                 uint16_t deadlineUs, uint8_t intent);

// Close the session, restoring the saved map. `newMap` is the four bus ids to
// commit instead of the saved ones (the `axis_map`-as-exit route), or nullptr
// for `probe_end`'s "put it back the way it was".
//
// Refuses while the switch is open: exiting there restores the axis map and
// leaves the tool pressed into the bed. That is PROBE_NOT_CLEARED, not an exit.
bool probeExit(const uint8_t* newMap);

// Poll a leg in flight. No-op unless one is. Call from the Core 0 loop.
void probeTick(void);

// True while this module holds a session. Lets the gates refuse without reading
// machineState, which anything may write.
bool probeActive(void);

// True while a leg is actually executing — the window in which the bus is
// starved and no supervision can run.
bool probeLegInFlight(void);

// The last leg's outcome, for `getstate`. PROBE_* from ipc/core1_rpc.h.
uint8_t probeLastCause(void);
uint8_t probeLastRetries(void);
int32_t probeLastSteps(void);
