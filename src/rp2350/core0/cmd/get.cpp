// get.cpp — `get <key> …`: one key=value per key, in the order asked, all
// from one snapshot. Bare `get` lists the keys this build knows.
//
// Markers in place of a value: `?` the read failed, `-` the key does not
// apply now, `!` the key is not recognised (so a newer host still reads the
// other fields from older firmware). Memory reads only: bus transactions stay
// their own commands. A group name (`status`) asks for its keys.

#include <Arduino.h>
#include "table.h"
#include "../ops/position.h"
#include "../ops/homing.h"
#include "../ops/probe.h"
#include "../ops/frames.h"
#include "../ops/mesh.h"
#include "../ops/machine.h"
#include "../ops/sequence.h"
#include "../ops/tools.h"
#include "../ops/bus.h"
#include "../status.h"
#include "../data_plane.h"
#include "../planner/queue.h"
#include "../../ipc/shared_state.h"
#include "../config/machine_cfg.h"

namespace {

// Everything a key can report, read up front so the reply describes one
// instant rather than the span of the print.
struct Snap {
    uint8_t  state, enabled, homed, job, latched;
    uint16_t faults;
    uint8_t  homingReason, homeFailWhy, homeFailNode, homeCycle;
    uint8_t  probingReason, probeCause, probeRetries, ringOwner;
    int32_t  probeSteps;
    bool     probed;
    int32_t  pz;
    uint16_t nodeHomed, nodeEnabled;
    uint16_t buf, mute, excluded, touched;
    uint32_t queuedUs;
    uint8_t  expectedSeq;
    int32_t  pos[4];
    float    mpos[4], wpos[4];
    bool     mposOk[4], wposOk[4];
    uint8_t  head;
    uint8_t  tools[CFG_MAX_HEADS], headCount;
    uint32_t late, aMiss;
    uint8_t  meshFile;
    bool     meshOn;
    uint16_t meshNx, meshNy;
    float    meshSlope;
    char     seqFail[48];
    uint8_t  seq, seqAborted;
    bool     resumeOk;
    uint16_t resumeLayer;
    uint32_t resumeRecord;
    uint8_t  liftReason;
    uint16_t liftCount;
    bool     powered, toolOn;
    uint32_t onMs;
};

// The duty's on-time since its reset, the open span included; 0 while a reset
// is pending on Core 1. Read again if Core 1 switched the tool meanwhile.
uint32_t dutyOnTime() {
    if (dutySeen != dutyEpoch) return 0;
    uint32_t closed, at;
    bool on;
    do {
        closed = dutyOnMs;
        on     = toolOn;
        at     = toolOnAtMs;
    } while (closed != dutyOnMs || on != toolOn);
    return closed + (on ? millis() - at : 0);
}

void takeSnap(Snap& s) {
    s.state         = machineState;
    s.enabled       = axes_enabled;
    s.homed         = axes_homed;
    s.faults        = machineFaults();
    s.job           = jobReason;
    s.resumeOk      = plannerResumePoint(&s.resumeLayer, &s.resumeRecord);
    s.latched       = homingLatched;
    s.homingReason  = homingReason;
    s.homeFailWhy   = homingFailWhy();
    s.homeFailNode  = homingFailNode();
    s.homeCycle     = homingHeld();
    s.probingReason = probingReason;
    s.ringOwner = ringOwner;
    s.headCount = machineCfgValid() ? machineCfg().headCount : 0;
    for (uint8_t h = 0; h < s.headCount; h++) s.tools[h] = toolsMounted(h);
    s.probeCause    = probeLastCause();
    s.probeRetries  = probeLastRetries();
    s.probeSteps    = probeLastSteps();
    s.probed        = probeValid(axisNode(SLOT_Z), &s.pz);
    s.nodeHomed     = originMask();
    s.nodeEnabled   = nodeEnabled;
    s.buf           = getBufCount();
    s.mute          = busMute();
    s.excluded      = busExcluded();
    s.touched       = busTouched();
    s.expectedSeq   = dataPlaneExpectedSeq();
    {
        // Under the lock: Core 1 claims and pops.
        const uint32_t k = spin_lock_blocking(plannerLock);
        const float q = plannerRing.queuedTime();
        s.liftReason = jobLift.last;
        s.liftCount  = jobLift.count;
        s.powered    = jobPower.node != 0;
        spin_unlock(plannerLock, k);
        s.queuedUs = (uint32_t)(q * 1e6f);
    }
    for (uint8_t i = 0; i < 4; i++) s.pos[i] = machinePos[i];
    for (uint8_t i = 0; i < 4; i++) {
        s.mposOk[i] = framesMPos(i, &s.mpos[i]);
        s.wposOk[i] = framesWPos(i, &s.wpos[i]);
    }
    s.head = framesSelected();
    s.late = plannerExec.lateAdoptions();
    s.aMiss = plannerExec.aMisses();
    s.meshFile = meshFile();
    s.meshOn   = meshEnabled();
    s.meshNx   = meshNx();
    s.meshNy   = meshNy();
    s.meshSlope = meshSlope();
    s.seq        = sequenceId();
    s.seqAborted = sequenceAbortedId();
    snprintf(s.seqFail, sizeof s.seqFail, "%s", sequenceFailWhy());
    s.toolOn = toolOn;
    s.onMs   = dutyOnTime();
}

enum KeyResult : uint8_t { KEY_OK, KEY_NA, KEY_FAILED };

// Formats one value into `out`, or says why there is none.
typedef KeyResult (*KeyFn)(const Snap&, char* out, size_t n);

bool homeFailed(const Snap& s) {
    return (s.faults & F_HOMING) && s.homeFailWhy != LEGFAIL_NONE;
}
bool probeReported(const Snap& s) {
    return s.state == STATE_PROBING || (s.faults & F_PROBE);
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

// LiftReason bits by name, comma-separated; `-` for none.
KeyResult fmtLift(char* out, size_t n, uint8_t mask) {
    static const char* const kNames[8] = {
        "end", "corner", "duty", "deadline", "unwind", "pause", "abort", "dry",
    };
    if (!mask) return KEY_NA;
    size_t at = 0;
    for (uint8_t i = 0; i < 8 && at < n; i++)
        if (mask & (1u << i)) at += snprintf(out + at, n - at, at ? ",%s" : "%s", kNames[i]);
    return KEY_OK;
}

KeyResult fmtMesh(char* out, size_t n, const Snap& s) {
    if (s.meshFile == MESH_FILE_ABSENT) snprintf(out, n, "flat");
    else if (s.meshFile == MESH_FILE_BAD) snprintf(out, n, "bad");
    else if (!s.meshOn) snprintf(out, n, "off");
    else snprintf(out, n, "%ux%u", s.meshNx, s.meshNy);
    return KEY_OK;
}

struct Key { const char* name; KeyFn fn; };

const Key kKeys[] = {
    { "state",     [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.state); } },
    { "enabled",   [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.enabled, 2); } },
    { "homed",     [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.homed, 2); } },
    { "faults",    [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.faults, 4); } },
    { "job",       [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_JOB ? fmtU(o, n, s.job) : KEY_NA; } },
    // Where a paused job resumes: the LAYER_HDR's layer and record.
    { "resume_layer",  [](const Snap& s, char* o, size_t n) {
        return s.resumeOk ? fmtU(o, n, s.resumeLayer) : KEY_NA; } },
    { "resume_record", [](const Snap& s, char* o, size_t n) {
        return s.resumeOk ? fmtU(o, n, (unsigned)s.resumeRecord) : KEY_NA; } },
    // In a job: the latest lift's reasons (LiftReason) and the lifts since it
    // opened; the powered tool's switch and its duty on-time, ms.
    { "liftreason", [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_JOB ? fmtLift(o, n, s.liftReason) : KEY_NA; } },
    { "liftcount", [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_JOB ? fmtU(o, n, s.liftCount) : KEY_NA; } },
    { "toolon",    [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_JOB && s.powered ? fmtU(o, n, s.toolOn ? 1 : 0) : KEY_NA; } },
    { "onms",      [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_JOB && s.powered ? fmtU(o, n, (unsigned)s.onMs) : KEY_NA; } },
    { "latched",   [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.latched, 2); } },
    // See homing.h for what the codes point at; homenode is 0 for a dummy leg.
    { "homefail",  [](const Snap& s, char* o, size_t n) {
        return homeFailed(s) ? fmtU(o, n, s.homeFailWhy) : KEY_NA; } },
    { "homenode",  [](const Snap& s, char* o, size_t n) {
        return homeFailed(s) ? fmtU(o, n, s.homeFailNode) : KEY_NA; } },
    // The live sequence (SequenceId), else the one a sequence failure ended.
    { "seq",       [](const Snap& s, char* o, size_t n) {
        if (s.seq != SEQ_NONE) return fmtU(o, n, s.seq);
        if ((s.faults & (F_SEQUENCE | F_HOMING | F_PROBE)) && s.seqAborted != SEQ_NONE) return fmtU(o, n, s.seqAborted);
        return KEY_NA; } },
    // What refused a sequence after its ok (sequence.h).
    { "seqfail",   [](const Snap& s, char* o, size_t n) {
        if (!(s.faults & F_SEQUENCE)) return KEY_NA;
        snprintf(o, n, "%s", s.seqFail); return KEY_OK; } },
    { "cfgerr",    [](const Snap& s, char* o, size_t n) {
        if (!(s.faults & F_CONFIG)) return KEY_NA;
        snprintf(o, n, "%s", machineCfgBlockName()); return KEY_OK; } },
    { "homing",    [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_HOMING ? fmtU(o, n, s.homingReason) : KEY_NA; } },
    { "jogging",   [](const Snap& s, char* o, size_t n) {
        return s.state == STATE_JOGGING ? fmtU(o, n, s.ringOwner) : KEY_NA; } },
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
    // Enabled nodes, mapped or not; `enabled` covers bound axes only.
    { "nodeenabled", [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.nodeEnabled, 3); } },
    // Bus ids, bit n = node n (ops/bus.h).
    { "mute",      [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.mute, 3); } },
    { "excluded",  [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.excluded, 3); } },
    { "touched",   [](const Snap& s, char* o, size_t n) { return fmtHex(o, n, s.touched, 3); } },
    // Planner blocks queued, and the motion time of those not yet started, µs.
    { "buf",       [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.buf); } },
    { "queued",    [](const Snap& s, char* o, size_t n) { return fmtU(o, n, (unsigned)s.queuedUs); } },
    // The next wire seq the data plane will execute; not flow control.
    { "expectedseq", [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.expectedSeq); } },
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
    // The tool id each head has mounted, head order, 0 for none.
    { "tools",     [](const Snap& s, char* o, size_t n) {
        if (s.headCount == 0) return KEY_NA;
        size_t k = 0;
        for (uint8_t h = 0; h < s.headCount && k < n; h++)
            k += snprintf(o + k, n - k, h ? ",%u" : "%u", s.tools[h]);
        return KEY_OK; } },
    // Planner blocks Core 1 found late, since boot; each held the motion briefly.
    { "late",      [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.late); } },
    // Tangential curves whose tracked A missed the planned end A, since boot.
    { "amiss",     [](const Snap& s, char* o, size_t n) { return fmtU(o, n, s.aMiss); } },
    // The bed mesh: <nx>x<ny> loaded and on, off, flat (no file) or bad.
    { "mesh",      [](const Snap& s, char* o, size_t n) { return fmtMesh(o, n, s); } },
    // Its steepest slope between neighbouring points, mm/mm.
    { "meshslope", [](const Snap& s, char* o, size_t n) {
        if (s.meshFile != MESH_FILE_OK) return KEY_NA;
        snprintf(o, n, "%.5f", s.meshSlope); return KEY_OK; } },
};
constexpr uint8_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);
constexpr uint8_t KEY_UNKNOWN = 0xFF;

uint8_t findKey(const char* name, size_t len);

// A group stands for its keys, in order. Group and key names share one namespace.
const char* const kStatusKeys[] = {
    "state", "homed", "enabled", "nodehomed", "nodeenabled", "head", "tools",
    "pos", "mpos", "wpos", "buf", "mute", "excluded", "touched", "mesh",
};
struct Group { const char* name; const char* const* keys; uint8_t count; };
const Group kGroups[] = {
    { "status", kStatusKeys, sizeof(kStatusKeys) / sizeof(kStatusKeys[0]) },
};

const Group* findGroup(const char* name, size_t len) {
    for (const Group& g : kGroups)
        if (strlen(g.name) == len && strncmp(g.name, name, len) == 0) return &g;
    return nullptr;
}

// The keys of one request; a key asked for twice is printed once, where first asked.
struct Request {
    const char* names[GET_MAX_KEYS];
    uint8_t idx[GET_MAX_KEYS], lens[GET_MAX_KEYS];
    uint8_t count = 0;

    // False when the request is full.
    bool add(const char* name, size_t len) {
        if (len > 255) len = 255;
        for (uint8_t i = 0; i < count; i++)
            if (lens[i] == len && strncmp(names[i], name, len) == 0) return true;
        if (count == GET_MAX_KEYS) return false;
        names[count] = name;
        lens[count]  = len;
        idx[count]   = findKey(name, len);
        count++;
        return true;
    }
};

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

    Request r;
    for (const char* p = args; *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != ' ') p++;
        const size_t len = p - start;
        bool ok = true;
        if (const Group* g = findGroup(start, len)) {
            for (uint8_t i = 0; i < g->count && ok; i++)
                ok = r.add(g->keys[i], strlen(g->keys[i]));
        } else {
            ok = r.add(start, len);
        }
        if (!ok) { Serial.println("err too_many_keys"); return true; }
    }

    Snap s;
    takeSnap(s);
    printKeys(s, r.idx, r.names, r.lens, r.count);
    return true;
}

// `?` — the status group.
bool cmdQuery(const char*) { return cmdGet("status"); }
