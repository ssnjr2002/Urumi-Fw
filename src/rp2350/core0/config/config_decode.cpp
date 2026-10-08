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

    if (cycle.isNull()) out->cycle = defaultCycle;
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

// ─── Validation ───────────────────────────────────────────────────────────────

static bool positive(float v) { return isfinite(v) && v > 0.0f; }

static CfgDecodeError checkHoming(const CfgAxis& a) {
    const CfgHoming& h = a.homing;
    if (!h.present) return CFG_DEC_OK;
    if (h.cycle == 0 || !positive(h.startFeed)) return CFG_DEC_HOMING;
    if (a.rotary) {
        if (!positive(h.budgetRevs) || !positive(h.sweepFeed) ||
            !positive(h.toleranceDeg) || !isfinite(h.indexPos)) return CFG_DEC_HOMING;
        return CFG_DEC_OK;
    }
    // A seek budget is sized from maxTravel, so a linear home needs one.
    if (!positive(a.maxTravel) || !isfinite(h.seekScaler) || h.seekScaler < 1.0f ||
        !positive(h.seekFeed) || !positive(h.latchFeed) ||
        !positive(h.backoffDist) || !positive(h.pullOffDist) ||
        (h.hasParkPos && !isfinite(h.parkPos))) return CFG_DEC_HOMING;
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
// anchor's range shifted by the head offset); park and load within the
// anchor's range.
static CfgDecodeError checkFrames(const MachineCfg& c) {
    bool anchor = c.laserNode != 0;
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgHead& hd = c.heads[h];
        if (!isfinite(hd.xOffset) || !isfinite(hd.yOffset) || !isfinite(c.workZ[h]))
            return CFG_DEC_FRAMES;
        if (hd.xOffset == 0.0f && hd.yOffset == 0.0f) anchor = true;
        if (hd.hasProbeSwitch &&
            (!inRange(c.x, hd.probeSwitch.x - hd.xOffset) ||
             !inRange(c.y, hd.probeSwitch.y - hd.yOffset))) return CFG_DEC_FRAMES;
    }
    if (!anchor || !isfinite(c.workX) || !isfinite(c.workY)) return CFG_DEC_FRAMES;
    if (c.hasPark && (!inRange(c.x, c.park.x) || !inRange(c.y, c.park.y))) return CFG_DEC_FRAMES;
    if (c.hasLoad && (!inRange(c.x, c.load.x) || !inRange(c.y, c.load.y))) return CFG_DEC_FRAMES;
    return CFG_DEC_OK;
}

static CfgDecodeError validate(const MachineCfg& c) {
    const CfgAxis* axes[2 + 2 * CFG_MAX_HEADS];
    uint8_t n = 0;
    axes[n++] = &c.x;
    axes[n++] = &c.y;
    for (uint8_t h = 0; h < c.headCount; h++) {
        axes[n++] = &c.heads[h].z;
        axes[n++] = &c.heads[h].a;
    }
    for (uint8_t i = 0; i < n; i++) {
        CfgDecodeError e = checkAxis(*axes[i]);
        if (e != CFG_DEC_OK) return e;
    }

    // Every node id — axes and peripherals — must be distinct, fitted or not:
    // one bus address cannot be two devices.
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
    return checkFrames(c);
}

// ─── Entry ────────────────────────────────────────────────────────────────────

CfgDecodeError configDecode(const uint8_t* blob, size_t len, MachineCfg* out) {
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
    fm["defaultHead"] = true;
    fm["laser"]       = true;
    fm["work"]        = true;
    fm["positions"]   = true;
    fm["jogUnhomed"]  = true;
    fm["meshOn"]      = true;
    filterNode(fm["peripherals"].to<JsonArray>().add<JsonObject>());

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
            !readOptPoint(heads[h]["probeSwitch"], &hd.hasProbeSwitch, &hd.probeSwitch))
            return CFG_DEC_MISSING;
    }

    // A laser is (0, 0) by definition; it carries only the node that switches it.
    JsonVariantConst laser = m["laser"];
    out->laserNode = 0;
    if (!laser.isNull()) {
        JsonVariantConst ln = laser["node"];
        if (!ln.is<unsigned>()) return CFG_DEC_MISSING;
        if (ln.as<unsigned>() < 1 || ln.as<unsigned>() > CFG_BUS_ADDR_MAX) return CFG_DEC_NODE_ID;
        out->laserNode = (uint8_t)ln.as<unsigned>();
    }

    JsonObjectConst work = m["work"];
    JsonArrayConst workZ = work["z"];
    if (work.isNull() || !readFloat(work["x"], &out->workX) ||
        !readFloat(work["y"], &out->workY) || workZ.isNull()) return CFG_DEC_MISSING;
    if (workZ.size() != out->headCount) return CFG_DEC_FRAMES;
    for (uint8_t h = 0; h < out->headCount; h++)
        if (!readFloat(workZ[h], &out->workZ[h])) return CFG_DEC_MISSING;

    JsonObjectConst pos = m["positions"];
    if (pos.isNull() || !readOptPoint(pos["park"], &out->hasPark, &out->park) ||
        !readOptPoint(pos["load"], &out->hasLoad, &out->load)) return CFG_DEC_MISSING;

    if (!readBool(m["jogUnhomed"], &out->jogUnhomed) ||
        !readBool(m["meshOn"], &out->meshOn)) return CFG_DEC_MISSING;

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

    return validate(*out);
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
    }
    return "?";
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
