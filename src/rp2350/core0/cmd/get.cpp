// get.cpp — `get <key> …`: one key=value per key, in the order asked, all
// from one snapshot. Bare `get` lists the keys this build knows.
//
// Markers in place of a value: `?` the read failed, `-` the key does not
// apply now, `!` the key is not recognised (so a newer host still reads the
// other fields from older firmware). Memory reads only: bus transactions stay
// their own commands.

#include <Arduino.h>
#include "table.h"
#include "../ops/position.h"
#include "../ops/homing.h"
#include "../ops/probe.h"
#include "../ops/frames.h"
#include "../../ipc/shared_state.h"
#include "../config/machine_cfg.h"

namespace {

// Everything a key can report, read up front so the reply describes one
// instant rather than the span of the print.
struct Snap {
    uint8_t  state, enabled, homed, alarm, running, latched;
    uint8_t  homingReason, homeFailWhy, homeFailNode, homeCycle;
    uint8_t  probingReason, probeCause, probeRetries;
    int32_t  probeSteps;
    bool     probed;
    int32_t  pz;
    uint16_t nodeHomed;
    int32_t  pos[4];
    float    mpos[4], wpos[4];
    bool     mposOk[4], wposOk[4];
    uint8_t  head;
#ifdef DEBUG_TIMING
    uint32_t texp, tmeas, twall;
#endif
};

void takeSnap(Snap& s) {
    s.state         = machineState;
    s.enabled       = axes_enabled;
    s.homed         = axes_homed;
    s.alarm         = alarmReason;
    s.running       = runningReason;
    s.latched       = homingLatched;
    s.homingReason  = homingReason;
    s.homeFailWhy   = homingFailWhy();
    s.homeFailNode  = homingFailNode();
    s.homeCycle     = homingHeld();
    s.probingReason = probingReason;
    s.probeCause    = probeLastCause();
    s.probeRetries  = probeLastRetries();
    s.probeSteps    = probeLastSteps();
    s.probed        = probeValid(axisNode(SLOT_Z), &s.pz);
    s.nodeHomed     = originMask();
    for (uint8_t i = 0; i < 4; i++) s.pos[i] = machinePos[i];
    for (uint8_t i = 0; i < 4; i++) {
        s.mposOk[i] = framesMPos(i, &s.mpos[i]);
        s.wposOk[i] = framesWPos(i, &s.wpos[i]);
    }
    s.head = framesSelected();
#ifdef DEBUG_TIMING
    s.texp  = jobExpectedUs;
    s.tmeas = jobMeasuredUs;
    s.twall = jobWallUs;
#endif
}

enum KeyResult : uint8_t { KEY_OK, KEY_NA, KEY_FAILED };

// Formats one value into `out`, or says why there is none.
typedef KeyResult (*KeyFn)(const Snap&, char* out, size_t n);

bool homeFailed(const Snap& s) {
    return s.alarm == ALARM_HOMING_FAIL && s.homeFailWhy != LEGFAIL_NONE;
}
bool probeReported(const Snap& s) {
    return s.state == STATE_PROBING || s.alarm == ALARM_PROBE_FAIL;
}

KeyResult fmtU(char* out, size_t n, unsigned v)            { snprintf(out, n, "%u", v); return KEY_OK; }
KeyResult fmtL(char* out, size_t n, long v)                { snprintf(out, n, "%ld", v); return KEY_OK; }
KeyResult fmtHex(char* out, size_t n, unsigned v, int w)   { snprintf(out, n, "0x%0*x", w, v); return KEY_OK; }

// x,y,z,a in units, `-` for a slot with no value (an unbound Z/A, or X/Y
// with no head selected for wpos).
KeyResult fmtUnits(char* out, size_t n, const float* v, const bool* ok) {
    size_t at = 0;
    for (uint8_t i = 0; i < 4 && at < n; i++) {
        const char* sep = i ? "," : "";
        at += ok[i] ? snprintf(out + at, n - at, "%s%.3f", sep, v[i])
                    : snprintf(out + at, n - at, "%s-", sep);
    }
    return KEY_OK;
}

struct Key { const char* name; KeyFn fn; };

const Key kKeys[] = {
    { "state",     [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.state); } },
    { "enabled",   [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.enabled, 2); } },
    { "homed",     [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.homed, 2); } },
    { "alarm",     [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.alarm); } },
    { "running",   [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.running); } },
    { "latched",   [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.latched, 2); } },
    // See homing.h for what the codes point at; homenode is 0 for a dummy leg.
    { "homefail",  [](const Snap& s, char* o, size_t n) {
        return homeFailed(s) ? fmtU(o, n, s.homeFailWhy) : KEY_NA; } },
    { "homenode",  [](const Snap& s, char* o, size_t n) {
        return homeFailed(s) ? fmtU(o, n, s.homeFailNode) : KEY_NA; } },
    { "cfgerr",    [](const Snap& s, char* o, size_t n) {
        if (s.alarm != ALARM_CONFIG) return KEY_NA;
        snprintf(o, n, "%s", machineCfgBlockName()); return KEY_OK; } },
    { "homing",    [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_HOMING ? fmtU(o, n, s.homingReason) : KEY_NA; } },
    // `probing` is the session phase; `probe`, `retries` and `psteps` are the
    // last leg's outcome, the only report of a leg boundary.
    { "probing",   [](const Snap& s, char* o, size_t n) {
        return probeReported(s) ? fmtU(o, n, s.probingReason) : KEY_NA; } },
    { "probe",     [](const Snap& s, char* o, size_t n) {
        return probeReported(s) ? fmtU(o, n, s.probeCause) : KEY_NA; } },
    { "retries",   [](const Snap& s, char* o, size_t n) {
        return probeReported(s) ? fmtU(o, n, s.probeRetries) : KEY_NA; } },
    { "psteps",    [](const Snap& s, char* o, size_t n) {
        return probeReported(s) ? fmtL(o, n, s.probeSteps) : KEY_NA; } },
    // The contact height of the Z in slot 2, when it holds one.
    { "probed",    [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.probed ? 1 : 0); } },
    { "pz",        [](const Snap& s, char* o, size_t n) {
        return s.probed ? fmtL(o, n, s.pz) : KEY_NA; } },
    { "homecycle", [](const Snap& s, char* o, size_t n) {
        return s.homeCycle ? fmtU(o, n, s.homeCycle) : KEY_NA; } },
    // Nodes holding an origin, mapped or not; `homed` covers bound axes only.
    { "nodehomed", [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.nodeHomed, 3); } },
    // machinePos in steps, slot order. Never a sentinel: gate on `homed`.
    { "pos",       [](const Snap& s, char* o, size_t n) {
        snprintf(o, n, "%ld,%ld,%ld,%ld", (long)s.pos[0], (long)s.pos[1],
                 (long)s.pos[2], (long)s.pos[3]);
        return KEY_OK; } },
    // Machine and work position, units (docs/plans/coordinate-system.md).
    { "mpos",      [](const Snap& s, char* o, size_t n) { return fmtUnits(o, n, s.mpos, s.mposOk); } },
    { "wpos",      [](const Snap& s, char* o, size_t n) { return fmtUnits(o, n, s.wpos, s.wposOk); } },
    // The controlled point: a head index, `anchor` (the laser), `-` for none.
    { "head",      [](const Snap& s, char* o, size_t n) {
        if (s.head == FRAMES_NONE) return KEY_NA;
        if (s.head == FRAMES_ANCHOR) { snprintf(o, n, "anchor"); return KEY_OK; }
        return fmtU(o, n, s.head); } },
#ifdef DEBUG_TIMING
    // Expected vs measured duration (us) of the last completed burst, and its
    // wall time including any pause inside it. tmeas > texp: Core 1 fell behind.
    { "texp",      [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.texp); } },
    { "tmeas",     [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.tmeas); } },
    { "twall",     [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.twall); } },
#endif
};
constexpr uint8_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
constexpr uint8_t KEY_UNKNOWN = 0xFF;

uint8_t findKey(const char* name, size_t len) {
    for (uint8_t i = 0; i < kKeyCount; i++)
        if (strlen(kKeys[i].name) == len && strncmp(kKeys[i].name, name, len) == 0) return i;
    return KEY_UNKNOWN;
}

// Prints `key=value` for each listed key, space-separated, then a newline.
void printKeys(const Snap& s, const uint8_t* idx, const char* const* names,
               const uint8_t* lens, uint8_t count) {
    bool first = true;
    for (uint8_t i = 0; i < count; i++) {
        char val[64];
        const char* v = val;
        if (idx[i] == KEY_UNKNOWN) {
            v = "!";
        } else {
            switch (kKeys[idx[i]].fn(s, val, sizeof(val))) {
                case KEY_OK:     break;
                case KEY_NA:     v = "-"; break;
                case KEY_FAILED: v = "?"; break;
            }
        }
        Serial.printf("%s%.*s=%s", first ? "" : " ", lens[i], names[i], v);
        first = false;
    }
    Serial.println();
}

} // namespace

bool cmdGet(const char* args) {
    if (*args == '\0') {
        Serial.print("keys");
        for (uint8_t i = 0; i < kKeyCount; i++) Serial.printf(" %s", kKeys[i].name);
        Serial.println();
        return true;
    }

    const char* names[GET_MAX_KEYS];
    uint8_t idx[GET_MAX_KEYS], lens[GET_MAX_KEYS];
    uint8_t count = 0;
    for (const char* p = args; *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') p++;
        if (count == GET_MAX_KEYS) { Serial.println("err too_many_keys"); return true; }
        size_t len = p - start;
        names[count] = start;
        lens[count]  = len > 255 ? 255 : len;
        idx[count]   = findKey(start, len);
        count++;
    }

    Snap s;
    takeSnap(s);
    printKeys(s, idx, names, lens, count);
    return true;
}
