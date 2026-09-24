// Config decode — see config_decode.h.

#include "config_decode.h"
#include <math.h>
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
    f["invert"]       = true;
    f["rotary"]       = true;
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

static bool readAxis(JsonObjectConst j, CfgAxis* out) {
    JsonObjectConst node = j["node"];
    if (node.isNull() || !readNode(node, &out->node)) return false;
    if (!readFloat(j["stepsPerUnit"], &out->stepsPerUnit)) return false;
    if (!readFloat(j["maxFeed"],      &out->maxFeed))      return false;
    if (!readFloat(j["maxAccel"],     &out->maxAccel))     return false;
    if (!readFloat(j["maxTravel"],    &out->maxTravel))    return false;
    if (!j["invert"].is<bool>() || !j["rotary"].is<bool>()) return false;
    out->invert = j["invert"].as<bool>();
    out->rotary = j["rotary"].as<bool>();
    return true;
}

// ─── Validation ───────────────────────────────────────────────────────────────

static CfgDecodeError checkAxis(const CfgAxis& a) {
    if (a.node.id < 1 || a.node.id > CFG_BUS_ADDR_MAX) return CFG_DEC_NODE_ID;
    if (a.node.type != CFG_NODE_STEPPER)               return CFG_DEC_NODE_TYPE;
    if (!isfinite(a.stepsPerUnit) || a.stepsPerUnit <= 0.0f) return CFG_DEC_STEPS;
    if (!isfinite(a.maxFeed)   || a.maxFeed   < 0.0f ||
        !isfinite(a.maxAccel)  || a.maxAccel  < 0.0f ||
        !isfinite(a.maxTravel) || a.maxTravel < 0.0f) return CFG_DEC_CEILING;
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
    return CFG_DEC_OK;
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
    fm["defaultHead"] = true;
    filterNode(fm["peripherals"].to<JsonArray>().add<JsonObject>());

    JsonDocument doc;
    if (deserializeMsgPack(doc, blob, len, DeserializationOption::Filter(filter)))
        return CFG_DEC_MSGPACK;

    JsonVariantConst v = doc["v"];
    if (!v.is<unsigned>() || v.as<unsigned>() != CFG_SCHEMA_VERSION) return CFG_DEC_VERSION;
    out->version = (uint16_t)v.as<unsigned>();

    JsonObjectConst m = doc["machine"];
    if (m.isNull()) return CFG_DEC_MISSING;
    if (!readAxis(m["x"], &out->x) || !readAxis(m["y"], &out->y)) return CFG_DEC_MISSING;

    JsonArrayConst heads = m["heads"];
    if (heads.isNull()) return CFG_DEC_MISSING;
    if (heads.size() == 0 || heads.size() > CFG_MAX_HEADS) return CFG_DEC_HEADS;
    out->headCount = (uint8_t)heads.size();
    for (uint8_t h = 0; h < out->headCount; h++) {
        if (!readAxis(heads[h]["z"], &out->heads[h].z) ||
            !readAxis(heads[h]["a"], &out->heads[h].a)) return CFG_DEC_MISSING;
    }

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
    }
    return "?";
}

void configSlotMap(const MachineCfg& cfg, uint8_t head, uint8_t none, uint8_t out[4]) {
    const CfgHead& h = cfg.heads[head];
    const CfgAxis* axes[4] = { &cfg.x, &cfg.y, &h.z, &h.a };
    for (int i = 0; i < 4; i++)
        out[i] = axes[i]->node.present ? axes[i]->node.id : none;
}
