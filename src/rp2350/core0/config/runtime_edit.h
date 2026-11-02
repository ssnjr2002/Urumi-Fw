#pragma once
#include <stddef.h>
#include <stdint.h>
#include "config_decode.h"

// ─────────────────────────────────────────────────────────────────────────────
// Editing a decoded config by field path (docs/plans/runtime-config.md), from
// the generated table in runtime_fields.h. A path is the blob's own, as
// `machine.heads.0.z.maxFeed` or `toolProfiles.knife.cutFeed`. Free of Arduino
// I/O so it builds under `pio test -e native`; the caller checks the machine
// state and runs the effects.
// ─────────────────────────────────────────────────────────────────────────────

enum RtError : uint8_t {
    RT_OK = 0,
    RT_NO_FIELD,    // no such field on this machine
    RT_BAD_VALUE,   // not a value of the field's type
    RT_RANGE,       // outside the table's limits
    RT_PRECISION,   // more digits than a float32 keeps
    RT_ABSENT,      // a probe switch height with no probe switch
    RT_RULE,        // a rule refused it (RtApplied::rule)
};

enum RtAxis : uint8_t { RT_AXIS_NONE, RT_AXIS_X, RT_AXIS_Y, RT_AXIS_Z, RT_AXIS_A };

// What an accepted set asks of its caller.
struct RtApplied {
    bool           changed;   // false: the field already held the value
    unsigned       effects;   // RtEffect bits of the field
    RtAxis         axis;      // the edited axis, for an axis field
    uint8_t        head;      // its head, for Z and A
    uint8_t        node;      // its node
    CfgDecodeError rule;      // the refusal, with RT_RULE
};

enum RtEncodeError : uint8_t {
    RT_ENC_OK = 0,
    RT_ENC_DECODE,    // the stored blob is not msgpack, or too large for RAM
    RT_ENC_TOO_BIG,   // the result does not fit
    RT_ENC_VERIFY,    // the result does not decode to the live values
};

// Set one field from its text: a number, true or false, or `x y` for a point.
// A refusal leaves `c` as it was.
RtError runtimeSet(MachineCfg& c, const char* path, size_t len, const char* text, RtApplied* out);

// Format one field: `-` for an absent one, `x,y` for a point. False if there is
// no such field.
bool runtimeGet(const MachineCfg& c, const char* path, size_t len, char* out, size_t n);

// The stored blob at buf[0..len) with every table field taken from `live`, back
// into buf (capacity `cap`). `check` is scratch for the decode that proves it.
RtEncodeError runtimeEncode(const MachineCfg& live, uint8_t* buf, size_t len, size_t cap,
                            MachineCfg* check, size_t* outLen);

// The shortest decimal that reads back as `f`, as a double.
double runtimeShortest(float f);

const char* runtimeErrorName(RtError e);
