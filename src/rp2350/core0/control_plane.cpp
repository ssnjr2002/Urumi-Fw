// Core 0 control plane: text command dispatch (docs/reference/commands.md).
// One command per line; replies with exactly one text line.
//
// Two tables: primitives (cmd/), atomic and config-free, and controller
// commands (controller/cmd/), which read the config and are refused without
// one. What is left here is the lookup and the kind's admission -- see
// cmd/table.h for why it is a table and not the chain of startsWith() it
// replaced.

#include <Arduino.h>
#include "control_plane.h"
#include "cmd/table.h"
#include "controller/cmd/table.h"
#include "config/machine_cfg.h"
#include "ops/machine.h"
#include <string.h>

// Order is irrelevant: the match is the whole command word, so no row can
// shadow another. Grouped to read alongside cmd/'s files, nothing more.
static const Cmd kCommands[] = {
    // query.cpp
    { "ping",         cmdPing,        K_READ },
    { "cfg",          cmdCfg,         K_READ },
    { "pingnode",     cmdPingNode,    K_READ },
    { "nodepos",      cmdNodePos,     K_READ },
    { "nodestat",     cmdNodeStat,    K_READ },
    { "busstat",      cmdBusStat,     K_READ },
    { "vac_switch",   cmdVacSwitch,   K_READ },
    // get.cpp
    { "get",          cmdGet,         K_READ },
    { "?",            cmdQuery,       K_READ },   // get status
    // lifecycle.cpp
    { "stop",         cmdStop,        K_ANY },
    { "unstop",       cmdUnstop,      K_CLEAR },
    { "reset",        cmdReset,       K_WRITE },
    { "rst",          cmdReset,       K_WRITE },
#ifdef PICO_ALLOW_UNCONFIGURED
    { "uncfg",        cmdUncfg,       K_WRITE },
#endif
    { "seqreset",     cmdSeqReset,    K_ANY },
    { "pause",        cmdPause,       K_CONTEXT },
    { "cancel",       cmdCancel,      K_CONTEXT },
    // periph.cpp
    { "vac_servo",    cmdVacServo,    K_WRITE },
    { "vac_pump",     cmdVacPump,     K_WRITE },
    { "knife_osc",    cmdKnifeOsc,    K_WRITE },
    { "knife_blower", cmdKnifeBlower, K_WRITE },
    { "laser",        cmdLaser,       K_WRITE },
    { "makesafe",     cmdMakeSafe,    K_WRITE },
    // axis.cpp
    { "slot_map",     cmdSlotMap,     K_WRITE | K_BARE_READ },
    { "axes_map",     cmdAxesMap,     K_WRITE | K_BARE_READ | K_IN_PROBE },   // a map ends a probe
    { "setorigin",    cmdSetOrigin,   K_WRITE | K_IN_HOMING },
    { "home_end",     cmdHomeEnd,     K_CONTEXT | K_IN_HOMING },
    { "step",         cmdStep,        K_WRITE },
    { "line",         cmdLine,        K_STEP },
    { "bez",          cmdBez,         K_STEP },
    { "jog",          cmdJog,         K_STEP | K_IN_TOOL | K_IN_PLACE },
    { "feed",         cmdFeed,        K_ANY },
    { "hallscan",     cmdHallScan,    K_WRITE },
    { "leg",          cmdLeg,         K_STEP | K_IN_HOMING | K_IN_PROBE },
    { "leg_abort",    cmdLegAbort,    K_ANY },
    { "dummy_leg",    cmdDummyLeg,    K_STEP | K_IN_HOMING },
    { "axes_enable",  cmdAxesEnable,  K_WRITE },
    { "bus_enable",   cmdBusEnable,   K_WRITE },
    { "bus_exclude",  cmdBusExclude,  K_ANY },
    { "enable",       cmdEnable,      K_WRITE },
    { "disable",      cmdDisable,     K_WRITE },
    { "probe_map",    cmdProbeMap,    K_CONTEXT },
    { "probe_end",    cmdProbeEnd,    K_CONTEXT | K_IN_PROBE },
    { "setprobe",     cmdSetProbe,    K_WRITE },
    { "unprobe",      cmdUnprobe,     K_ANY },
};

static const Cmd kControllerCommands[] = {
    { "unalarm",      cmdUnalarm,     K_CLEAR },
    { "home",         cmdHome,        K_SEQUENCE },
    { "goto", "probe", cmdGoto,       K_SEQUENCE | K_IN_TOOL },
    { "goto", "mount", cmdGoto,       K_SEQUENCE | K_IN_TOOL },
    { "goto",         cmdGoto,        K_SEQUENCE },
    { "select",       cmdSelect,      K_WRITE | K_IN_TOOL },
    { "wzero",        cmdWzero,       K_WRITE | K_IN_TOOL },
    { "wset",         cmdWset,        K_WRITE },
    { "wclear",       cmdWclear,      K_WRITE },
    { "mesh",         cmdMesh,        K_WRITE },
    { "mount",        cmdMount,       K_SEQUENCE | K_IN_TOOL },
    { "unmount",      cmdUnmount,     K_SEQUENCE | K_IN_TOOL },
    { "place",        cmdPlace,       K_CONTEXT },
    { "set",          cmdSet,         K_WRITE },
    { "save",         cmdSave,        K_WRITE },
};

// Whole word `w` (`len` long) is `name`.
static bool is(const char* w, size_t len, const char* name) {
    return strlen(name) == len && strncmp(w, name, len) == 0;
}

// The first row for the command word, its `sub` (if any) matching the second
// word; rows with a `sub` come before their command's catch-all.
template <size_t N>
static const Cmd* lookup(const Cmd (&table)[N], const char* s, size_t wordLen) {
    const char* w2 = s + wordLen;
    while (*w2 == ' ') w2++;
    size_t len2 = 0;
    while (w2[len2] && w2[len2] != ' ') len2++;
    for (size_t i = 0; i < N; i++)
        if (is(s, wordLen, table[i].name) && (!table[i].sub || is(w2, len2, table[i].sub)))
            return &table[i];
    return nullptr;
}

// Handle one control-plane text line. Replies with exactly one line per the wire
// contract (docs/reference/commands.md): `ok` / `err <reason>` / a typed read.
// Returns false if the line names no command -- the caller decides what to say.
bool handleCommand(const String& input) {
    const char* s = input.c_str();

    // The command word is everything up to the first space. Matching the WHOLE
    // word is what makes the table order-independent: `enable` cannot swallow
    // `axes_enable`, which under startsWith() dispatch was only prevented by
    // testing the longer one first.
    size_t wordLen = 0;
    while (s[wordLen] && s[wordLen] != ' ') wordLen++;
    if (wordLen == 0) return false;

    const Cmd* c = lookup(kCommands, s, wordLen);
    if (!c) {
        c = lookup(kControllerCommands, s, wordLen);
        if (!c) return false;
        if (!machineCfgValid()) { Serial.println("err unconfigured"); return true; }
    }

    const char* args = s + wordLen;        // hand the handler its arguments
    while (*args == ' ') args++;           // already positioned -- no offsets

    if (!((c->kind & K_BARE_READ) && *args == '\0')) {
        if (const char* why = machineAdmit(c->kind)) {
            Serial.printf("err %s\n", why);
            return true;
        }
    }
    return c->fn(args);
}
