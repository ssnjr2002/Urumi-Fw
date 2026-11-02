// Runtime config edits — see runtime_edit.h.

#include "runtime_edit.h"
#include "runtime_fields.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>
#include <utility>
#include <ArduinoJson.h>

// ─── The table ────────────────────────────────────────────────────────────────
// Every row of the generated table names a member of its scope's struct, of the
// row's type: a wrong one fails the build.

template <RtScope S> struct RtStruct;
template <> struct RtStruct<RT_SCOPE_MACHINE> { using type = MachineCfg; };
template <> struct RtStruct<RT_SCOPE_AXIS>    { using type = CfgAxis; };
template <> struct RtStruct<RT_SCOPE_HEAD>    { using type = CfgHead; };
template <> struct RtStruct<RT_SCOPE_TOOL>    { using type = CfgTool; };

template <RtType T, class M> struct RtFits : std::false_type {};
template <> struct RtFits<RT_TYPE_F32, float> : std::true_type {};
template <> struct RtFits<RT_TYPE_BOOL, bool> : std::true_type {};
template <> struct RtFits<RT_TYPE_U16, uint16_t> : std::true_type {};
template <> struct RtFits<RT_TYPE_POINT, CfgPoint> : std::true_type {};
template <> struct RtFits<RT_TYPE_POINT, CfgProbeSwitch> : std::true_type {};

#define RT_MEMBER(scope, member) decltype(std::declval<RtStruct<scope>::type&>().member)
#define RT_CHECK_REQUIRED(scope, path, member, ty, ...) \
    static_assert(RtFits<ty, RT_MEMBER(scope, member)>::value, path);
#define RT_CHECK_OPTIONAL(scope, path, member, has, ty, ...) \
    static_assert(RtFits<ty, RT_MEMBER(scope, member)>::value, path); \
    static_assert(std::is_same<RT_MEMBER(scope, has), bool>::value, path);
RUNTIME_FIELDS(RT_CHECK_REQUIRED, RT_CHECK_OPTIONAL)

// A point is written through its first two floats, whichever struct it is.
static_assert(offsetof(CfgPoint, y) == sizeof(float), "x, y");
static_assert(offsetof(CfgProbeSwitch, x) == 0 && offsetof(CfgProbeSwitch, y) == sizeof(float), "x, y");

struct RtRow {
    RtScope     scope;
    const char* path;
    uint16_t    offset;
    int16_t     hasOffset;   // -1 on a required field
    RtType      type;
    float       min, max;    // NAN: none
    bool        minExclusive;
    RtHoming    homing;
    unsigned    effects;
};

#define RT_OFFSET(scope, member) ((int)offsetof(RtStruct<scope>::type, member))
#define RT_ROW_REQUIRED(scope, path, member, ty, min, max, ex, homing, effects) \
    {scope, path, (uint16_t)RT_OFFSET(scope, member), -1, ty, min, max, ex, homing, effects},
#define RT_ROW_OPTIONAL(scope, path, member, has, ty, min, max, ex, homing, effects) \
    {scope, path, (uint16_t)RT_OFFSET(scope, member), (int16_t)RT_OFFSET(scope, has), ty, min, max, ex, homing, effects},
static const RtRow kRows[] = {RUNTIME_FIELDS(RT_ROW_REQUIRED, RT_ROW_OPTIONAL)};

static size_t valueSize(RtType t) {
    switch (t) {
        case RT_TYPE_F32:   return sizeof(float);
        case RT_TYPE_BOOL:  return sizeof(bool);
        case RT_TYPE_U16:   return sizeof(uint16_t);
        case RT_TYPE_POINT: return 2 * sizeof(float);
    }
    return 0;
}

static bool homingHas(const RtRow& r, const CfgAxis& a) {
    if (r.homing == RT_HOMING_NONE) return true;
    if (!a.homing.present) return false;
    return r.homing == RT_HOMING_BOTH || (r.homing == RT_HOMING_ROTARY) == a.rotary;
}

// ─── Floats ───────────────────────────────────────────────────────────────────

static void formatShortest(float f, char* out, size_t n) {
    for (int prec = 1; prec <= 9; prec++) {
        snprintf(out, n, "%.*g", prec, (double)f);
        if (strtof(out, nullptr) != f) continue;
        // %g writes 10 at one digit as 1e+01.
        if (strchr(out, 'e') && fabsf(f) >= 1.0f && fabsf(f) < 1e9f) snprintf(out, n, "%.0f", (double)f);
        return;
    }
}

double runtimeShortest(float f) {
    char b[32];
    formatShortest(f, b, sizeof b);
    return strtod(b, nullptr);
}

// ─── Paths ────────────────────────────────────────────────────────────────────

struct RtTarget {
    const RtRow* row;
    uint8_t*     base;   // the scope's struct
    RtAxis       axis;
    uint8_t      head;
    uint8_t      node;
};

static bool eat(const char*& p, const char* end, const char* word) {
    const size_t n = strlen(word);
    if ((size_t)(end - p) < n || strncmp(p, word, n) != 0) return false;
    p += n;
    return true;
}

static bool resolve(MachineCfg& c, const char* path, size_t len, RtTarget* t) {
    const char* p = path;
    const char* const end = path + len;
    RtScope scope;
    const CfgAxis* axis = nullptr;
    *t = RtTarget{nullptr, nullptr, RT_AXIS_NONE, 0, 0};

    if (eat(p, end, "toolProfiles.")) {
        const char* dot = (const char*)memchr(p, '.', end - p);
        if (!dot) return false;
        for (uint8_t i = 0; i < c.toolCount && !t->base; i++) {
            const char* name = c.tools[i].name;
            if (strlen(name) == (size_t)(dot - p) && strncmp(name, p, dot - p) == 0)
                t->base = (uint8_t*)&c.tools[i];
        }
        if (!t->base) return false;
        p = dot + 1;
        scope = RT_SCOPE_TOOL;
    } else if (eat(p, end, "machine.")) {
        scope = RT_SCOPE_AXIS;
        CfgAxis* a = nullptr;
        if (eat(p, end, "x."))      { a = &c.x; t->axis = RT_AXIS_X; }
        else if (eat(p, end, "y.")) { a = &c.y; t->axis = RT_AXIS_Y; }
        else if (eat(p, end, "heads.")) {
            if (end - p < 2 || p[0] < '0' || p[0] > '9' || p[1] != '.') return false;
            const uint8_t h = (uint8_t)(p[0] - '0');
            if (h >= c.headCount) return false;
            p += 2;
            t->head = h;
            if (eat(p, end, "z."))      { a = &c.heads[h].z; t->axis = RT_AXIS_Z; }
            else if (eat(p, end, "a.")) { a = &c.heads[h].a; t->axis = RT_AXIS_A; }
            else { scope = RT_SCOPE_HEAD; t->base = (uint8_t*)&c.heads[h]; }
        } else {
            scope = RT_SCOPE_MACHINE;
            t->base = (uint8_t*)&c;
        }
        if (a) { t->base = (uint8_t*)a; t->node = a->node.id; axis = a; }
    } else {
        return false;
    }

    const size_t rest = end - p;
    for (const RtRow& r : kRows) {
        if (r.scope != scope || strlen(r.path) != rest || strncmp(r.path, p, rest) != 0) continue;
        if (axis && !homingHas(r, *axis)) return false;
        t->row = &r;
        return true;
    }
    return false;
}

// ─── Set ──────────────────────────────────────────────────────────────────────

static bool parseFloat(const char* text, const char** end, float* out, RtError* e) {
    char* stop;
    const double d = strtod(text, &stop);
    if (stop == text || !isfinite(d)) { *e = RT_BAD_VALUE; return false; }
    const float f = (float)d;
    if (!isfinite(f) || runtimeShortest(f) != d) { *e = RT_PRECISION; return false; }
    *out = f + 0.0f;   // no -0
    *end = stop;
    return true;
}

static bool inRange(const RtRow& r, float v) {
    if (!isnan(r.min) && (r.minExclusive ? v <= r.min : v < r.min)) return false;
    return isnan(r.max) || v <= r.max;
}

static bool blank(const char* p) {
    while (*p == ' ') p++;
    return *p == '\0';
}

// The new value's bytes into `out`.
static RtError parseValue(const RtRow& r, const char* text, uint8_t* out) {
    RtError e = RT_BAD_VALUE;
    const char* end = text;
    switch (r.type) {
        case RT_TYPE_F32: {
            float v;
            if (!parseFloat(text, &end, &v, &e)) return e;
            if (!blank(end)) return RT_BAD_VALUE;
            if (!inRange(r, v)) return RT_RANGE;
            memcpy(out, &v, sizeof v);
            return RT_OK;
        }
        case RT_TYPE_POINT: {
            float v[2];
            if (!parseFloat(text, &end, &v[0], &e)) return e;
            if (!parseFloat(end, &end, &v[1], &e)) return e;
            if (!blank(end)) return RT_BAD_VALUE;
            if (!inRange(r, v[0]) || !inRange(r, v[1])) return RT_RANGE;
            memcpy(out, v, sizeof v);
            return RT_OK;
        }
        case RT_TYPE_BOOL: {
            const size_t n = strcspn(text, " ");
            const bool on = n == 4 && strncmp(text, "true", 4) == 0;
            if ((!on && !(n == 5 && strncmp(text, "false", 5) == 0)) || !blank(text + n))
                return RT_BAD_VALUE;
            memcpy(out, &on, sizeof on);
            return RT_OK;
        }
        case RT_TYPE_U16: {
            char* stop;
            const unsigned long v = strtoul(text, &stop, 10);
            if (stop == text || text[0] == '-' || !blank(stop)) return RT_BAD_VALUE;
            if (v > 0xFFFFul || !inRange(r, (float)v)) return RT_RANGE;
            const uint16_t u = (uint16_t)v;
            memcpy(out, &u, sizeof u);
            return RT_OK;
        }
    }
    return RT_BAD_VALUE;
}

RtError runtimeSet(MachineCfg& c, const char* path, size_t len, const char* text, RtApplied* out) {
    *out = RtApplied{false, 0, RT_AXIS_NONE, 0, 0, CFG_DEC_OK};
    RtTarget t;
    if (!resolve(c, path, len, &t)) return RT_NO_FIELD;
    const RtRow& r = *t.row;

    // The one row inside another optional one: the blob's `z` needs its point.
    if (r.scope == RT_SCOPE_HEAD && r.hasOffset == (int16_t)RT_OFFSET(RT_SCOPE_HEAD, probeSwitch.hasZ) &&
        !((CfgHead*)t.base)->hasProbeSwitch) return RT_ABSENT;

    uint8_t next[2 * sizeof(float)], old[2 * sizeof(float)];
    const RtError e = parseValue(r, text, next);
    if (e != RT_OK) return e;

    const size_t n = valueSize(r.type);
    uint8_t* value = t.base + r.offset;
    bool* has = r.hasOffset >= 0 ? (bool*)(t.base + r.hasOffset) : nullptr;
    const bool hadIt = !has || *has;
    if (hadIt && memcmp(value, next, n) == 0) return RT_OK;

    memcpy(old, value, n);
    memcpy(value, next, n);
    if (has) *has = true;
    const CfgDecodeError rule = configValidate(c);
    if (rule != CFG_DEC_OK) {
        memcpy(value, old, n);
        if (has) *has = hadIt;
        out->rule = rule;
        return RT_RULE;
    }
    out->changed = true;
    out->effects = r.effects;
    out->axis    = t.axis;
    out->head    = t.head;
    out->node    = t.node;
    return RT_OK;
}

// ─── Get ──────────────────────────────────────────────────────────────────────

bool runtimeGet(const MachineCfg& c, const char* path, size_t len, char* out, size_t n) {
    RtTarget t;
    if (!resolve(const_cast<MachineCfg&>(c), path, len, &t)) return false;
    const RtRow& r = *t.row;
    const uint8_t* value = t.base + r.offset;
    if (r.hasOffset >= 0 && !*(const bool*)(t.base + r.hasOffset)) {
        snprintf(out, n, "-");
        return true;
    }
    float f[2];
    switch (r.type) {
        case RT_TYPE_F32:
            memcpy(f, value, sizeof(float));
            formatShortest(f[0], out, n);
            break;
        case RT_TYPE_POINT: {
            memcpy(f, value, sizeof f);
            formatShortest(f[0], out, n);
            const size_t at = strlen(out);
            if (at + 1 < n) {
                out[at] = ',';
                formatShortest(f[1], out + at + 1, n - at - 1);
            }
            break;
        }
        case RT_TYPE_BOOL:
            snprintf(out, n, "%s", *(const bool*)value ? "true" : "false");
            break;
        case RT_TYPE_U16: {
            uint16_t u;
            memcpy(&u, value, sizeof u);
            snprintf(out, n, "%u", (unsigned)u);
            break;
        }
    }
    return true;
}

// ─── Encode ───────────────────────────────────────────────────────────────────

// Every field of `c`, with the index of its axis (x, y, then each head's z, a),
// head or tool. Rows an axis's homing lacks are left out.
template <class F>
static void eachField(const MachineCfg& c, F fn) {
    for (const RtRow& r : kRows) {
        switch (r.scope) {
            case RT_SCOPE_MACHINE:
                fn(r, (const uint8_t*)&c, 0);
                break;
            case RT_SCOPE_AXIS: {
                const uint8_t axes = (uint8_t)(2 + 2 * c.headCount);
                for (uint8_t i = 0; i < axes; i++) {
                    const CfgAxis& a = i == 0 ? c.x : i == 1 ? c.y
                                     : i % 2 == 0 ? c.heads[i / 2 - 1].z : c.heads[i / 2 - 1].a;
                    if (homingHas(r, a)) fn(r, (const uint8_t*)&a, i);
                }
                break;
            }
            case RT_SCOPE_HEAD:
                for (uint8_t h = 0; h < c.headCount; h++) fn(r, (const uint8_t*)&c.heads[h], h);
                break;
            case RT_SCOPE_TOOL:
                for (uint8_t t = 0; t < c.toolCount; t++) fn(r, (const uint8_t*)&c.tools[t], t);
                break;
        }
    }
}

static JsonObject scopeObject(JsonDocument& doc, const MachineCfg& c, RtScope scope, uint8_t i) {
    JsonObject m = doc["machine"];
    switch (scope) {
        case RT_SCOPE_MACHINE: return m;
        case RT_SCOPE_AXIS:
            if (i < 2) return m[i == 0 ? "x" : "y"];
            return m["heads"][i / 2 - 1][i % 2 == 0 ? "z" : "a"];
        case RT_SCOPE_HEAD: return m["heads"][i];
        case RT_SCOPE_TOOL: return doc["toolProfiles"][c.tools[i].name];
    }
    return JsonObject();
}

// The object holding a dotted path's last key, which *leaf is set to. Null if
// a level is missing and `create` is false.
static JsonObject descend(JsonObject o, const char* path, const char** leaf, bool create) {
    const char* dot;
    while ((dot = strchr(path, '.')) != nullptr) {
        char key[24];
        snprintf(key, sizeof key, "%.*s", (int)(dot - path), path);
        JsonObject next = o[key];
        if (next.isNull() && create) next = o[key].to<JsonObject>();
        if (next.isNull()) return next;
        o = next;
        path = dot + 1;
    }
    *leaf = path;
    return o;
}

// Writes one field. A double is enough: the serializer writes a whole value as
// an integer, and any other that fits a float32 as a float32.
static void storeField(JsonObject scope, const RtRow& r, const uint8_t* base) {
    const uint8_t* value = base + r.offset;
    const bool present = r.hasOffset < 0 || *(const bool*)(base + r.hasOffset);
    const char* leaf;
    JsonObject o = descend(scope, r.path, &leaf, present);
    if (o.isNull()) return;
    if (!present) { o.remove(leaf); return; }
    float f[2];
    switch (r.type) {
        case RT_TYPE_F32:
            memcpy(f, value, sizeof(float));
            o[leaf] = runtimeShortest(f[0]);
            break;
        case RT_TYPE_POINT: {
            memcpy(f, value, sizeof f);
            JsonObject p = o[leaf];
            if (p.isNull()) p = o[leaf].to<JsonObject>();
            p["x"] = runtimeShortest(f[0]);
            p["y"] = runtimeShortest(f[1]);
            break;
        }
        case RT_TYPE_BOOL:
            o[leaf] = *(const bool*)value;
            break;
        case RT_TYPE_U16: {
            uint16_t u;
            memcpy(&u, value, sizeof u);
            o[leaf] = u;
            break;
        }
    }
}

// Every table field of `b` holds what `a`'s does.
static bool sameFields(const MachineCfg& a, const MachineCfg& b) {
    bool same = a.headCount == b.headCount && a.toolCount == b.toolCount;
    if (!same) return false;
    eachField(a, [&](const RtRow& r, const uint8_t* base, uint8_t) {
        const uint8_t* other = (const uint8_t*)&b + (base - (const uint8_t*)&a);
        if (r.hasOffset >= 0) {
            const bool has = *(const bool*)(base + r.hasOffset);
            if (has != *(const bool*)(other + r.hasOffset)) { same = false; return; }
            if (!has) return;
        }
        if (memcmp(base + r.offset, other + r.offset, valueSize(r.type)) != 0) same = false;
    });
    return same;
}

RtEncodeError runtimeEncode(const MachineCfg& live, uint8_t* buf, size_t len, size_t cap,
                            MachineCfg* check, size_t* outLen) {
    JsonDocument doc;
    if (deserializeMsgPack(doc, (const uint8_t*)buf, len, DeserializationOption::NestingLimit(16)))
        return RT_ENC_DECODE;
    eachField(live, [&](const RtRow& r, const uint8_t* base, uint8_t i) {
        storeField(scopeObject(doc, live, r.scope, i), r, base);
    });
    if (doc.overflowed()) return RT_ENC_DECODE;
    const size_t need = measureMsgPack(doc);
    if (need > cap) return RT_ENC_TOO_BIG;
    serializeMsgPack(doc, buf, cap);
    *outLen = need;
    if (configDecode(buf, need, check) != CFG_DEC_OK || !sameFields(live, *check))
        return RT_ENC_VERIFY;
    return RT_ENC_OK;
}

const char* runtimeErrorName(RtError e) {
    switch (e) {
        case RT_OK:        return "ok";
        case RT_NO_FIELD:  return "no_field";
        case RT_BAD_VALUE: return "bad_value";
        case RT_RANGE:     return "range";
        case RT_PRECISION: return "precision";
        case RT_ABSENT:    return "absent";
        case RT_RULE:      return "rule";
    }
    return "?";
}
