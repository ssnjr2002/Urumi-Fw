// Config decode — see config_decode.h.

#include "config_decode.h"
#include <math.h>
#include <string.h>
#include <ArduinoJson.h>

// ─── Filter ───────────────────────────────────────────────────────────────────

static void filterNode(JsonObject f) {
    f["id"]      = true;
    f["type"]    = true;
    f["present"] = true;
}

static void filterAxis(JsonObject f) {
    filterNode(f["node"].to<JsonObject>());
    f["stepsPerUnit"] = true;
    f["maxFeed"]      = true;
    f["maxAccel"]     = true;
    f["maxTravel"]    = true;
    f["softLimits"]   = true;
    f["jogFeed"]      = true;
    f["jogFeedUnhomed"] = true;
    f["invertDir"]    = true;
    f["rotary"]       = true;
    JsonObject h = f["homing"].to<JsonObject>();
    for (const char* k : {"kind", "cycle", "rampSteps", "startFeed",
                          "seekPositive", "seekScaler", "seekFeed", "latchFeed",
                          "backoffDist", "pullOffDist", "parkPos",
                          "budgetRevs", "sweepFeed", "toleranceDeg", "indexPos"})
        h[k] = true;
}

// ─── Field readers ────────────────────────────────────────────────────────────
// Each returns false when the field is absent or has the wrong type.

static bool readNode(JsonObjectConst j, CfgNode* out) {
    JsonVariantConst id = j["id"], type = j["type"], present = j["present"];
    if (!id.is<unsigned>() || !type.is<unsigned>() || !present.is<bool>()) return false;
    unsigned idV = id.as<unsigned>();
    out->id      = idV > 0xFF ? 0 : (uint8_t)idV;   // 0 fails the range check
    out->type    = (uint8_t)type.as<unsigned>();
    out->present = present.as<bool>();
    return true;
}

static bool readFloat(JsonVariantConst v, float* out) {
    if (!v.is<float>()) return false;
    *out = v.as<float>();
    return true;
}

static bool readBool(JsonVariantConst v, bool* out) {
    if (!v.is<bool>()) return false;
    *out = v.as<bool>();
    return true;
}

static bool readPoint(JsonVariantConst v, CfgPoint* out) {
    return v.is<JsonObjectConst>() && readFloat(v["x"], &out->x) && readFloat(v["y"], &out->y);
}

// An optional point: absent is fine, present must be a point.
static bool readOptPoint(JsonVariantConst v, bool* has, CfgPoint* out) {
    *has = !v.isNull();
    return !*has || readPoint(v, out);
}

static bool readHoming(JsonObjectConst j, uint8_t defaultCycle, bool* rotaryKind,
                       CfgHoming* out) {
    *out = CfgHoming{};
    out->present = true;
    JsonVariantConst kind = j["kind"], cycle = j["cycle"], ramp = j["rampSteps"];
    if (kind.isNull())               *rotaryKind = false;
    else if (!kind.is<const char*>()) return false;
    else                             *rotaryKind = strcmp(kind.as<const char*>(), "rotary") == 0;

    out->hasCycle = !cycle.isNull();
    if (!out->hasCycle) out->cycle = defaultCycle;
    else if (!cycle.is<unsigned>() || cycle.as<unsigned>() > 0xFF) return false;
    else out->cycle = (uint8_t)cycle.as<unsigned>();
    if (!ramp.is<unsigned>() || ramp.as<unsigned>() > 0xFFFF) return false;
    out->rampSteps = (uint16_t)ramp.as<unsigned>();
    if (!readFloat(j["startFeed"], &out->startFeed)) return false;

    if (*rotaryKind) {
        return readFloat(j["budgetRevs"],   &out->budgetRevs) &&
               readFloat(j["sweepFeed"],    &out->sweepFeed) &&
               readFloat(j["toleranceDeg"], &out->toleranceDeg) &&
               readFloat(j["indexPos"],     &out->indexPos);
    }
    JsonVariantConst park = j["parkPos"];
    out->hasParkPos = !park.isNull();
    if (out->hasParkPos && !readFloat(park, &out->parkPos)) return false;
    return readBool(j["seekPositive"],  &out->seekPositive) &&
           readFloat(j["seekScaler"],  &out->seekScaler) &&
           readFloat(j["seekFeed"],    &out->seekFeed) &&
           readFloat(j["latchFeed"],   &out->latchFeed) &&
           readFloat(j["backoffDist"], &out->backoffDist) &&
           readFloat(j["pullOffDist"], &out->pullOffDist);
}

static CfgDecodeError readAxis(JsonObjectConst j, uint8_t defaultCycle, CfgAxis* out) {
    JsonObjectConst node = j["node"];
    if (node.isNull() || !readNode(node, &out->node) ||
        !readFloat(j["stepsPerUnit"], &out->stepsPerUnit) ||
        !readFloat(j["maxFeed"],      &out->maxFeed) ||
        !readFloat(j["maxAccel"],     &out->maxAccel) ||
        !readFloat(j["maxTravel"],    &out->maxTravel) ||
        !readFloat(j["jogFeed"],      &out->jogFeed) ||
        !readFloat(j["jogFeedUnhomed"], &out->jogFeedUnhomed) ||
        !readBool(j["softLimits"],    &out->softLimits) ||
        !readBool(j["invertDir"],     &out->invertDir) ||
        !readBool(j["rotary"],        &out->rotary)) return CFG_DEC_MISSING;
    out->homing = CfgHoming{};
    JsonObjectConst h = j["homing"];
    if (h.isNull()) return CFG_DEC_OK;
    bool rotaryKind;
    if (!readHoming(h, defaultCycle, &rotaryKind, &out->homing)) return CFG_DEC_MISSING;
    return rotaryKind == out->rotary ? CFG_DEC_OK : CFG_DEC_HOMING;
}

// An optional number: absent is 0.
static bool readOptFloat(JsonVariantConst v, bool* has, float* out) {
    *out = 0.0f;
    *has = !v.isNull();
    return !*has || readFloat(v, out);
}

static CfgDecodeError readTools(JsonObjectConst tools, MachineCfg* out) {
    out->toolCount = 0;
    if (tools.isNull()) return CFG_DEC_MISSING;
    if (tools.size() > CFG_MAX_TOOLS) return CFG_DEC_TOOLS;
    for (JsonPairConst kv : tools) {
        JsonObjectConst j = kv.value();
        CfgTool& t = out->tools[out->toolCount++];
        t = CfgTool{};
        if (strlen(kv.key().c_str()) >= sizeof t.name) return CFG_DEC_TOOLS;
        strcpy(t.name, kv.key().c_str());
        JsonVariantConst id = j["id"], type = j["toolType"];
        JsonObjectConst off = j["toolOffset"];
        if (!id.is<unsigned>() || !type.is<unsigned>() || off.isNull() ||
            !readBool(j["tangential"], &t.tangential) ||
            !j["periodCount"].is<unsigned>() ||
            !readFloat(j["cornerAngleDeg"], &t.cornerAngleDeg) ||
            !readOptFloat(j["aMaxDeg"], &t.hasAMaxDeg, &t.aMaxDeg) ||
            !readOptFloat(j["aOffsetDeg"], &t.hasAOffsetDeg, &t.aOffsetDeg) ||
            !readOptFloat(j["offsetMm"], &t.hasOffsetMm, &t.offsetMm) ||
            !readBool(j["plunge"], &t.plunge) ||
            !readFloat(off["xOffset"], &t.toolOffset.xOffset) ||
            !readFloat(off["yOffset"], &t.toolOffset.yOffset) ||
            !readOptFloat(j["cutFeed"], &t.hasCutFeed, &t.cutFeed) ||
            !readOptFloat(j["plungeFeed"], &t.hasPlungeFeed, &t.plungeFeed)) return CFG_DEC_MISSING;
        if (id.as<unsigned>() > 0xFF || type.as<unsigned>() > 0xFF ||
            j["periodCount"].as<unsigned>() > 0xFF) return CFG_DEC_TOOLS;
        t.periodCount = (uint8_t)j["periodCount"].as<unsigned>();
        t.id   = (uint8_t)id.as<unsigned>();
        t.toolType = (uint8_t)type.as<unsigned>();
        JsonObjectConst pw = j["power"];
        if (!pw.isNull()) {
            JsonVariantConst id = pw["node"]["id"], nt = pw["node"]["type"];
            if (!id.is<unsigned>() || !nt.is<unsigned>() ||
                !readFloat(pw["settleS"], &t.power.settleS)) return CFG_DEC_MISSING;
            if (id.as<unsigned>() == 0 || id.as<unsigned>() > 0xFF || nt.as<unsigned>() > 0xFF)
                return CFG_DEC_POWER;
            t.power.node.id = (uint8_t)id.as<unsigned>();
            t.power.node.type = (uint8_t)nt.as<unsigned>();
            JsonObjectConst d = pw["dutyLimits"];
            t.power.hasDutyLimits = !d.isNull();
            if (t.power.hasDutyLimits &&
                (!readFloat(d["maxOnS"], &t.power.dutyLimits.maxOnS) ||
                 !readFloat(d["minOnS"], &t.power.dutyLimits.minOnS) ||
                 !readFloat(d["dwellS"], &t.power.dutyLimits.dwellS))) return CFG_DEC_MISSING;
        }
        JsonVariantConst ch = j["chamberOffsets"];
        if (ch.isNull()) continue;
        JsonArrayConst a = ch;
        if (a.isNull()) return CFG_DEC_MISSING;
        if (a.size() > CFG_MAX_CHAMBERS) return CFG_DEC_TOOLS;
        t.chamberCount = (uint8_t)a.size();
        for (uint8_t i = 0; i < t.chamberCount; i++)
            if (!readFloat(a[i], &t.chamberOffsets[i])) return CFG_DEC_MISSING;
    }
    return CFG_DEC_OK;
}

// ─── Validation ───────────────────────────────────────────────────────────────

static bool positive(float v) { return isfinite(v) && v > 0.0f; }

static CfgDecodeError checkHoming(const CfgAxis& a) {
    const CfgHoming& h = a.homing;
    if (!h.present) return CFG_DEC_OK;
    if (h.cycle == 0 || !positive(h.startFeed)) return CFG_DEC_HOMING;
    if (a.rotary) {
        if (!positive(h.budgetRevs) || !positive(h.sweepFeed) ||
            !positive(h.toleranceDeg) || !isfinite(h.indexPos) ||
            (a.maxFeed > 0.0f && h.sweepFeed > a.maxFeed)) return CFG_DEC_HOMING;
        return CFG_DEC_OK;
    }
    // A seek budget is sized from maxTravel, so a linear home needs one.
    if (!positive(a.maxTravel) || !isfinite(h.seekScaler) || h.seekScaler < 1.0f ||
        !positive(h.seekFeed) || !positive(h.latchFeed) ||
        !positive(h.backoffDist) || !positive(h.pullOffDist) ||
        (h.hasParkPos && !isfinite(h.parkPos)) ||
        h.latchFeed >= h.seekFeed ||
        (a.maxFeed > 0.0f && h.seekFeed > a.maxFeed)) return CFG_DEC_HOMING;
    return CFG_DEC_OK;
}

static CfgDecodeError checkAxis(const CfgAxis& a) {
    if (a.node.id < 1 || a.node.id > CFG_BUS_ADDR_MAX) return CFG_DEC_NODE_ID;
    if (a.node.type != CFG_NODE_STEPPER)               return CFG_DEC_NODE_TYPE;
    if (!isfinite(a.stepsPerUnit) || a.stepsPerUnit <= 0.0f) return CFG_DEC_STEPS;
    if (!isfinite(a.maxFeed)   || a.maxFeed   < 0.0f ||
        !isfinite(a.maxAccel)  || a.maxAccel  < 0.0f ||
        !isfinite(a.maxTravel) || a.maxTravel < 0.0f ||
        !isfinite(a.jogFeed)   || a.jogFeed   < 0.0f ||
        !isfinite(a.jogFeedUnhomed) || a.jogFeedUnhomed < 0.0f ||
        (!a.rotary && a.maxTravel == 0.0f)) return CFG_DEC_CEILING;
    return checkHoming(a);
}

// A point inside [lo, hi] on an axis with a range; an axis without one
// (no linear home) cannot judge it.
static bool inRange(const CfgAxis& a, float v) {
    float lo, hi;
    return !configAxisRange(a, &lo, &hi) || (v >= lo && v <= hi);
}

// An anchor at (0, 0); every head's probe switch within its tip's reach (the
// anchor's range shifted by the head offset) at a height >= 0; park and load within the
// anchor's range.
static CfgDecodeError checkFrames(const MachineCfg& c) {
    bool anchor = c.laser.node != 0;
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgHead& hd = c.heads[h];
        if (!isfinite(hd.xOffset) || !isfinite(hd.yOffset))
            return CFG_DEC_FRAMES;
        if (hd.xOffset == 0.0f && hd.yOffset == 0.0f) anchor = true;
        if (hd.hasProbeSwitch &&
            (!inRange(c.x, hd.probeSwitch.x - hd.xOffset) ||
             !inRange(c.y, hd.probeSwitch.y - hd.yOffset))) return CFG_DEC_FRAMES;
        if (hd.hasProbeSwitch && hd.probeSwitch.hasZ &&
            !(isfinite(hd.probeSwitch.z) && hd.probeSwitch.z >= 0)) return CFG_DEC_FRAMES;
    }
    if (!anchor || !isfinite(c.work.x) || !isfinite(c.work.y)) return CFG_DEC_FRAMES;
    const CfgPositions& p = c.positions;
    if (p.hasPark && (!inRange(c.x, p.park.x) || !inRange(c.y, p.park.y))) return CFG_DEC_FRAMES;
    if (p.hasLoad && (!inRange(c.x, p.load.x) || !inRange(c.y, p.load.y))) return CFG_DEC_FRAMES;
    if (p.hasMount && (!inRange(c.x, p.mount.x) || !inRange(c.y, p.mount.y))) return CFG_DEC_FRAMES;
    return CFG_DEC_OK;
}

static bool feed(float v) { return isfinite(v) && v >= 0.0f; }

static CfgDecodeError checkTools(const MachineCfg& c) {
    for (uint8_t i = 0; i < c.toolCount; i++) {
        const CfgTool& t = c.tools[i];
        if (t.id == 0 || !isfinite(t.toolOffset.xOffset) || !isfinite(t.toolOffset.yOffset) ||
            !feed(t.cutFeed) || !feed(t.plungeFeed) || t.periodCount == 0 ||
            !(t.cornerAngleDeg >= 0 && t.cornerAngleDeg <= 180) ||
            !(isfinite(t.aMaxDeg) && t.aMaxDeg >= 0) ||
            !isfinite(t.aOffsetDeg) || (!t.tangential && t.aOffsetDeg != 0) ||
            !(isfinite(t.offsetMm) && t.offsetMm >= 0) || (!t.tangential && t.offsetMm != 0) ||
            (t.tangential && t.chamberCount > 0) ||
            (!t.tangential && t.periodCount != 1)) return CFG_DEC_TOOLS;
        for (uint8_t k = 0; k < t.chamberCount; k++)
            if (!isfinite(t.chamberOffsets[k])) return CFG_DEC_TOOLS;
        for (uint8_t k = 0; k < i; k++)
            if (c.tools[k].id == t.id) return CFG_DEC_TOOLS;
    }
    return CFG_DEC_OK;
}

// A powered tool's node: a present knife oscillator peripheral, or (bench
// builds) the laser's stepper node.
static CfgDecodeError checkPower(const MachineCfg& c) {
    for (uint8_t i = 0; i < c.toolCount; i++) {
        const CfgPower& p = c.tools[i].power;
        if (p.node.id == 0) continue;
        bool ok = false;
        for (uint8_t k = 0; k < c.periphCount; k++) {
            const CfgNode& n = c.peripherals[k];
            if (n.id == p.node.id && n.present && n.type == p.node.type &&
                n.type == CFG_NODE_KNIFE_OSC) ok = true;
        }
#ifdef ACCEPT_LASER_AS_POWER_TOOL
        if (c.laser.node != 0 && p.node.id == c.laser.node && p.node.type == CFG_NODE_STEPPER) ok = true;
#endif
        if (!ok) return CFG_DEC_POWER;
        if (!(p.settleS >= 0.0f) || !isfinite(p.settleS)) return CFG_DEC_POWER;
        const CfgDutyLimits& d = p.dutyLimits;
        if (p.hasDutyLimits && (!positive(d.maxOnS) || !positive(d.minOnS) ||
                                !positive(d.dwellS) || d.minOnS >= d.maxOnS)) return CFG_DEC_POWER;
    }
    return CFG_DEC_OK;
}

static uint8_t allAxes(const MachineCfg& c, const CfgAxis** axes) {
    uint8_t n = 0;
    axes[n++] = &c.x;
    axes[n++] = &c.y;
    for (uint8_t h = 0; h < c.headCount; h++) {
        axes[n++] = &c.heads[h].z;
        axes[n++] = &c.heads[h].a;
    }
    return n;
}

static CfgDecodeError checkUnlockRanges(const MachineCfg& c) {
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgHead& hd = c.heads[h];
        if (!(isfinite(hd.unlockRange) && hd.unlockRange >= 0) ||
            (hd.unlockRange > 0 && !hd.a.node.present)) return CFG_DEC_HEADS;
    }
    return CFG_DEC_OK;
}

static CfgDecodeError checkAxes(const MachineCfg& c) {
    const CfgAxis* axes[2 + 2 * CFG_MAX_HEADS];
    const uint8_t n = allAxes(c, axes);
    for (uint8_t i = 0; i < n; i++) {
        CfgDecodeError e = checkAxis(*axes[i]);
        if (e != CFG_DEC_OK) return e;
    }
    return CFG_DEC_OK;
}

// Every node id — axes and peripherals — must be distinct, fitted or not:
// one bus address cannot be two devices.
static CfgDecodeError checkNodeIds(const MachineCfg& c) {
    const CfgAxis* axes[2 + 2 * CFG_MAX_HEADS];
    const uint8_t n = allAxes(c, axes);
    uint16_t seen = 0;
    for (uint8_t i = 0; i < n; i++) {
        uint16_t bit = (uint16_t)(1u << axes[i]->node.id);
        if (seen & bit) return CFG_DEC_DUP_NODE;
        seen |= bit;
    }
    for (uint8_t i = 0; i < c.periphCount; i++) {
        uint8_t id = c.peripherals[i].id;
        if (id < 1 || id > CFG_BUS_ADDR_MAX) return CFG_DEC_NODE_ID;
        uint16_t bit = (uint16_t)(1u << id);
        if (seen & bit) return CFG_DEC_DUP_NODE;
        seen |= bit;
    }
    return CFG_DEC_OK;
}

static CfgDecodeError checkBuffer(const MachineCfg& c) {
    return positive(c.buffer.queueTime) && positive(c.buffer.idleTime) ? CFG_DEC_OK : CFG_DEC_BUFFER;
}

static CfgDecodeError checkFeeds(const MachineCfg& c) {
    const CfgFeed& fd = c.feed;
    for (float f : {fd.cutFallback, fd.travelDefault, fd.plungeFallback, fd.liftFeed})
        if (!(f >= 1.0f) || !isfinite(f)) return CFG_DEC_FEED;
    return CFG_DEC_OK;
}

static CfgDecodeError checkZClearance(const MachineCfg& c) {
    return c.zClearance >= 0.0f && isfinite(c.zClearance) ? CFG_DEC_OK : CFG_DEC_Z_CLEARANCE;
}

static CfgDecodeError checkOvercut(const MachineCfg& c) {
    return c.maxOvercut >= 0.0f && isfinite(c.maxOvercut) ? CFG_DEC_OK : CFG_DEC_OVERCUT;
}

// In the order their errors take precedence.
static CfgDecodeError (*const RULES[])(const MachineCfg&) = {
    checkUnlockRanges,
    checkAxes,
    checkNodeIds,
    checkBuffer,
    checkFeeds,
    checkZClearance,
    checkOvercut,
    checkTools,
    checkPower,
    checkFrames,
};

CfgDecodeError configValidate(const MachineCfg& c) {
    for (auto rule : RULES) {
        CfgDecodeError e = rule(c);
        if (e != CFG_DEC_OK) return e;
    }
    return CFG_DEC_OK;
}

// ─── Entry ────────────────────────────────────────────────────────────────────

CfgDecodeError configDecode(const uint8_t* blob, size_t len, MachineCfg* out) {
    CfgDecodeError e = configParse(blob, len, out);
    return e != CFG_DEC_OK ? e : configValidate(*out);
}

CfgDecodeError configParse(const uint8_t* blob, size_t len, MachineCfg* out) {
    JsonDocument filter;
    filter["v"] = true;
    JsonObject fm = filter["machine"].to<JsonObject>();
    filterAxis(fm["x"].to<JsonObject>());
    filterAxis(fm["y"].to<JsonObject>());
    JsonObject fh = fm["heads"].to<JsonArray>().add<JsonObject>();
    filterAxis(fh["z"].to<JsonObject>());
    filterAxis(fh["a"].to<JsonObject>());
    fh["xOffset"]     = true;
    fh["yOffset"]     = true;
    fh["probeSwitch"] = true;
    fh["unlockRange"] = true;
    fm["defaultHead"] = true;
    fm["laser"]       = true;
    fm["work"]        = true;
    fm["positions"]   = true;
    fm["jogUnhomed"]  = true;
    fm["meshOn"]      = true;
    fm["buffer"]      = true;
    fm["feed"]        = true;
    fm["zClearance"]  = true;
    fm["maxOvercut"]  = true;
    filterNode(fm["peripherals"].to<JsonArray>().add<JsonObject>());
    JsonObject ft = filter["toolProfiles"]["*"].to<JsonObject>();
    for (const char* k : {"id", "toolType", "tangential", "periodCount", "cornerAngleDeg",
                          "aMaxDeg", "aOffsetDeg", "offsetMm", "plunge", "toolOffset", "chamberOffsets", "cutFeed",
                          "plungeFeed", "power"})
        ft[k] = true;

    JsonDocument doc;
    if (deserializeMsgPack(doc, blob, len, DeserializationOption::Filter(filter)))
        return CFG_DEC_MSGPACK;

    JsonVariantConst v = doc["v"];
    if (!v.is<unsigned>() || v.as<unsigned>() != CFG_SCHEMA_VERSION) return CFG_DEC_VERSION;
    out->version = (uint16_t)v.as<unsigned>();

    JsonObjectConst m = doc["machine"];
    if (m.isNull()) return CFG_DEC_MISSING;
    CfgDecodeError e;
    if ((e = readAxis(m["x"], 2, &out->x)) != CFG_DEC_OK) return e;
    if ((e = readAxis(m["y"], 2, &out->y)) != CFG_DEC_OK) return e;

    JsonArrayConst heads = m["heads"];
    if (heads.isNull()) return CFG_DEC_MISSING;
    if (heads.size() == 0 || heads.size() > CFG_MAX_HEADS) return CFG_DEC_HEADS;
    out->headCount = (uint8_t)heads.size();
    for (uint8_t h = 0; h < out->headCount; h++) {
        CfgHead& hd = out->heads[h];
        if ((e = readAxis(heads[h]["z"], 1, &hd.z)) != CFG_DEC_OK) return e;
        if ((e = readAxis(heads[h]["a"], 2, &hd.a)) != CFG_DEC_OK) return e;
        if (!readFloat(heads[h]["xOffset"], &hd.xOffset) ||
            !readFloat(heads[h]["yOffset"], &hd.yOffset) ||
            !readOptFloat(heads[h]["unlockRange"], &hd.hasUnlockRange, &hd.unlockRange))
            return CFG_DEC_MISSING;
        JsonVariantConst ps = heads[h]["probeSwitch"];
        hd.hasProbeSwitch = !ps.isNull();
        if (hd.hasProbeSwitch &&
            (!ps.is<JsonObjectConst>() || !readFloat(ps["x"], &hd.probeSwitch.x) ||
             !readFloat(ps["y"], &hd.probeSwitch.y))) return CFG_DEC_MISSING;
        JsonVariantConst sz = ps["z"];
        hd.probeSwitch.hasZ = hd.hasProbeSwitch && !sz.isNull();
        if (hd.probeSwitch.hasZ && !readFloat(sz, &hd.probeSwitch.z)) return CFG_DEC_MISSING;
    }

    // A laser is (0, 0) by definition; it carries only the node that switches it.
    JsonVariantConst laser = m["laser"];
    out->laser.node = 0;
    if (!laser.isNull()) {
        JsonVariantConst ln = laser["node"];
        if (!ln.is<unsigned>()) return CFG_DEC_MISSING;
        if (ln.as<unsigned>() < 1 || ln.as<unsigned>() > CFG_BUS_ADDR_MAX) return CFG_DEC_NODE_ID;
        out->laser.node = (uint8_t)ln.as<unsigned>();
    }

    JsonObjectConst work = m["work"];
    if (work.isNull() || !readFloat(work["x"], &out->work.x) ||
        !readFloat(work["y"], &out->work.y)) return CFG_DEC_MISSING;

    JsonObjectConst pos = m["positions"];
    CfgPositions& p = out->positions;
    if (pos.isNull() || !readOptPoint(pos["park"], &p.hasPark, &p.park) ||
        !readOptPoint(pos["load"], &p.hasLoad, &p.load) ||
        !readOptPoint(pos["mount"], &p.hasMount, &p.mount)) return CFG_DEC_MISSING;

    if (!readBool(m["jogUnhomed"], &out->jogUnhomed) ||
        !readBool(m["meshOn"], &out->meshOn)) return CFG_DEC_MISSING;

    JsonObjectConst buf = m["buffer"];
    if (buf.isNull() || !readFloat(buf["queueTime"], &out->buffer.queueTime) ||
        !readFloat(buf["idleTime"], &out->buffer.idleTime)) return CFG_DEC_MISSING;

    JsonObjectConst fd = m["feed"];
    if (fd.isNull() || !readFloat(fd["cutFallback"], &out->feed.cutFallback) ||
        !readFloat(fd["travelDefault"], &out->feed.travelDefault) ||
        !readFloat(fd["plungeFallback"], &out->feed.plungeFallback) ||
        !readFloat(fd["liftFeed"], &out->feed.liftFeed) ||
        !readFloat(m["zClearance"], &out->zClearance) ||
        !readFloat(m["maxOvercut"], &out->maxOvercut)) return CFG_DEC_MISSING;

    JsonVariantConst dh = m["defaultHead"];
    if (!dh.is<unsigned>()) return CFG_DEC_MISSING;
    if (dh.as<unsigned>() >= out->headCount) return CFG_DEC_HEADS;
    out->defaultHead = (uint8_t)dh.as<unsigned>();

    JsonArrayConst periph = m["peripherals"];
    if (periph.isNull()) return CFG_DEC_MISSING;
    if (periph.size() > CFG_MAX_PERIPH) return CFG_DEC_PERIPH;
    out->periphCount = (uint8_t)periph.size();
    for (uint8_t i = 0; i < out->periphCount; i++) {
        if (!readNode(periph[i], &out->peripherals[i])) return CFG_DEC_MISSING;
    }
    return readTools(doc["toolProfiles"], out);
}

const char* configDecodeErrorName(CfgDecodeError e) {
    switch (e) {
        case CFG_DEC_OK:        return "ok";
        case CFG_DEC_MSGPACK:   return "msgpack";
        case CFG_DEC_VERSION:   return "version";
        case CFG_DEC_MISSING:   return "missing";
        case CFG_DEC_HEADS:     return "heads";
        case CFG_DEC_PERIPH:    return "periph";
        case CFG_DEC_NODE_ID:   return "node_id";
        case CFG_DEC_NODE_TYPE: return "node_type";
        case CFG_DEC_DUP_NODE:  return "dup_node";
        case CFG_DEC_STEPS:     return "steps";
        case CFG_DEC_CEILING:   return "ceiling";
        case CFG_DEC_HOMING:    return "homing";
        case CFG_DEC_FRAMES:    return "frames";
        case CFG_DEC_BUFFER:    return "buffer";
        case CFG_DEC_TOOLS:     return "tools";
        case CFG_DEC_FEED:      return "feed";
        case CFG_DEC_Z_CLEARANCE: return "z_clearance";
        case CFG_DEC_OVERCUT:     return "overcut";
        case CFG_DEC_POWER:     return "power";
    }
    return "?";
}

const CfgTool* configTool(const MachineCfg& cfg, uint8_t id) {
    for (uint8_t i = 0; i < cfg.toolCount; i++)
        if (cfg.tools[i].id == id) return &cfg.tools[i];
    return nullptr;
}

float configParkPos(const CfgAxis& a) {
    const CfgHoming& h = a.homing;
    return h.hasParkPos ? h.parkPos : h.seekPositive ? a.maxTravel : h.pullOffDist;
}

bool configAxisRange(const CfgAxis& a, float* lo, float* hi) {
    if (!a.homing.present || a.rotary) return false;
    const float park = configParkPos(a);
    *lo = a.homing.seekPositive ? park - a.maxTravel : park;
    *hi = a.homing.seekPositive ? park : park + a.maxTravel;
    return true;
}

void configSlotMap(const MachineCfg& cfg, uint8_t head, uint8_t none, uint8_t out[4]) {
    const CfgHead& h = cfg.heads[head];
    const CfgAxis* axes[4] = { &cfg.x, &cfg.y, &h.z, &h.a };
    for (int i = 0; i < 4; i++)
        out[i] = axes[i]->node.present ? axes[i]->node.id : none;
}
