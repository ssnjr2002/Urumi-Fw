#pragma once

#include <stdint.h>

// table.h — the command table and its handler declarations.
//
// WHY A TABLE. The old dispatch was a chain of input.startsWith(), which made
// correctness depend on ORDER: `axes_enable` worked only because it was tested
// before `enable`, and `bus_enable` likewise. Any new command sharing a prefix
// with an earlier one was silently swallowed, and nothing in the source said so.
// Here the match is the whole command word, so no entry can shadow another and
// the rows may be listed in any order.
//
// The second thing it removes is the hand-maintained arg offset. Every handler
// used to call argAfter(input, N) with N equal to strlen of its own name --
// twenty-odd magic numbers, each a silent bug on rename. Handlers now receive
// `args` already positioned.
//
// NO `gate` FIELD, on purpose. See gate.h.

struct Cmd {
    const char* name;
    bool (*fn)(const char* args);   // args: past the command word and its spaces
};

// ─── query.cpp — reads; nothing here changes machine state ────────────────────
bool cmdPing(const char*);
bool cmdStatus(const char*);        // also serves the `?` alias
bool cmdCfg(const char*);
bool cmdPingNode(const char*);
bool cmdNodePos(const char*);
bool cmdNodeStat(const char*);
bool cmdBusStat(const char*);
bool cmdVacSwitch(const char*);

// ─── get.cpp — key=value reads from one snapshot ───────────────────────────────
constexpr uint8_t GET_MAX_KEYS = 32;   // more in one request: err too_many_keys
bool cmdGet(const char*);

// ─── lifecycle.cpp — state transitions; unstop also makes the bus safe ─────────
bool cmdStop(const char*);
bool cmdUnstop(const char*);
bool cmdReset(const char*);
#ifdef PICO_ALLOW_UNCONFIGURED
bool cmdUncfg(const char*);
#endif
bool cmdSeqReset(const char*);
bool cmdPause(const char*);
bool cmdResume(const char*);
bool cmdCancel(const char*);

// ─── periph.cpp — one generic relay behind five rows ──────────────────────────
bool cmdVacServo(const char*);
bool cmdVacPump(const char*);
bool cmdKnifeOsc(const char*);
bool cmdKnifeBlower(const char*);
bool cmdLaser(const char*);
bool cmdMakeSafe(const char*);

// ─── axis.cpp — the only command file that WRITES the position model ──────────
bool cmdSlotMap(const char*);
bool cmdAxesMap(const char*);
bool cmdSetOrigin(const char*);
bool cmdHomeEnd(const char*);
bool cmdStep(const char*);
bool cmdLine(const char*);
bool cmdBez(const char*);
bool cmdFeed(const char*);
bool cmdHallScan(const char*);
bool cmdLeg(const char*);
bool cmdLegAbort(const char*);
bool cmdDummyLeg(const char*);
bool cmdAxesEnable(const char*);
bool cmdBusEnable(const char*);
bool cmdBusExclude(const char*);
bool cmdEnable(const char*);
bool cmdDisable(const char*);

// ─── the probe session (core0/ops/probe.cpp) ──────────────────────────────────
// Here rather than in a file of their own because probe_map writes the slot map
// and axis.cpp is the only command file that writes the position model. The
// session state and its supervisor live in core0/ops/probe.cpp; these are the three
// handlers that reach it.
bool cmdProbeMap(const char*);
bool cmdProbeLeg(const char*);
bool cmdProbeEnd(const char*);
bool cmdSetProbe(const char*);
bool cmdUnprobe(const char*);
