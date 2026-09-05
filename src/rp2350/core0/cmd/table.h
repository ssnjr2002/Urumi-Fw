#pragma once

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
bool cmdGetState(const char*);
bool cmdGetPos(const char*);
bool cmdStatus(const char*);        // also serves `status cfg` and the `?` alias
bool cmdPingNode(const char*);
bool cmdNodePos(const char*);
bool cmdNodeStat(const char*);
bool cmdVacSwitch(const char*);

// ─── lifecycle.cpp — state transitions only; no transport, no position ────────
bool cmdStop(const char*);
bool cmdReset(const char*);
bool cmdSeqReset(const char*);
bool cmdPause(const char*);
bool cmdResume(const char*);
bool cmdCancel(const char*);
bool cmdUnalarm(const char*);

// ─── periph.cpp — one generic relay behind five rows ──────────────────────────
bool cmdVacServo(const char*);
bool cmdVacPump(const char*);
bool cmdKnifeOsc(const char*);
bool cmdKnifeBlower(const char*);
bool cmdLaser(const char*);

// ─── axis.cpp — the only file that WRITES the position model ──────────────────
bool cmdAxisMap(const char*);
bool cmdSetOrigin(const char*);
bool cmdStep(const char*);
bool cmdHallScan(const char*);
bool cmdLinLeg(const char*);
bool cmdRotLeg(const char*);
bool cmdAxesEnable(const char*);
bool cmdBusEnable(const char*);
bool cmdEnable(const char*);
bool cmdDisable(const char*);
