#pragma once
#include <stddef.h>
#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// Config decode — the msgpack blob (web/src/machine/json/blob.ts) → MachineCfg.
//
// Only the fields the Pico consumes are decoded; the rest of the host's
// config is skipped by an ArduinoJson filter. The blob is the host's RESOLVED
// config, so every field decoded here must be present: there is no defaults
// table on this side. Exceptions: the homing block (absent: not homeable),
// and inside it `cycle` and `parkPos`.
//
// Validation is consumer-scoped: it checks what would make the Pico's own use
// of a field wrong, and leaves every other judgement to the host's
// validate.ts. Free of Arduino I/O so it builds under `pio test -e native`.
// ─────────────────────────────────────────────────────────────────────────────

#define CFG_SCHEMA_VERSION 2u  // payload version the decoder understands (blob `v`)
#define CFG_MAX_HEADS      4u
#define CFG_MAX_PERIPH     8u
#define CFG_BUS_ADDR_MAX   8u   // must equal BUS_ADDR_MAX (shared_state.h)
#define CFG_NODE_STEPPER   0x01 // NODE_TYPE_STEPPER (include/common.h)

struct CfgNode {
    uint8_t id;
    uint8_t type;
    bool    present;
};

// The homing block (docs/plans/controller-homing.md). Feeds are mm/s or deg/s.
struct CfgHoming {
    bool     present;      // false: no switch or index, the axis cannot be homed
    uint8_t  cycle;        // lower homes first; absent: Z 1, everything else 2
    uint16_t rampSteps;
    float    startFeed;
    // linear
    bool     seekPositive; // home toward +
    bool     hasParkPos;
    float    seekScaler;   // seek budget = (maxTravel + pullOffDist) × this
    float    seekFeed;
    float    latchFeed;
    float    backoffDist;  // retract between the fast and slow approaches
    float    pullOffDist;  // final retract
    float    parkPos;      // coordinate after the pull-off; valid if hasParkPos
    // rotary
    float    budgetRevs;   // runaway ceiling for a sweep
    float    sweepFeed;
    float    toleranceDeg; // max disagreement of the two sweeps
    float    indexPos;     // coordinate of the index itself
};

struct CfgAxis {
    CfgNode   node;
    float     stepsPerUnit;
    float     maxFeed;    // 0 = uncapped
    float     maxAccel;   // 0 = uncapped
    float     maxTravel;  // usable length from the park position
    bool      softLimits; // enforce [park, park ± maxTravel]
    bool      invertDir;  // wiring fix so + is physical
    bool      rotary;
    CfgHoming homing;
};

struct CfgHead {
    CfgAxis z;
    CfgAxis a;
};

struct MachineCfg {
    uint16_t version;
    CfgAxis  x;
    CfgAxis  y;
    CfgHead  heads[CFG_MAX_HEADS];
    uint8_t  headCount;
    uint8_t  defaultHead;
    CfgNode  peripherals[CFG_MAX_PERIPH];
    uint8_t  periphCount;
};

enum CfgDecodeError : uint8_t {
    CFG_DEC_OK = 0,
    CFG_DEC_MSGPACK,        // not valid msgpack, or too large to decode
    CFG_DEC_VERSION,        // missing or unknown `v`
    CFG_DEC_MISSING,        // a consumed field is absent or has the wrong type
    CFG_DEC_HEADS,          // no heads, too many, or defaultHead out of range
    CFG_DEC_PERIPH,         // more peripherals than CFG_MAX_PERIPH
    CFG_DEC_NODE_ID,        // node id outside 1..CFG_BUS_ADDR_MAX
    CFG_DEC_NODE_TYPE,      // an axis node that is not a stepper
    CFG_DEC_DUP_NODE,       // one node id claimed twice
    CFG_DEC_STEPS,          // stepsPerUnit not a positive finite number
    CFG_DEC_CEILING,        // maxFeed / maxAccel / maxTravel negative or not finite
    CFG_DEC_HOMING,         // a homing field out of range, or the wrong kind for the axis
};

// Decode and validate `len` bytes at `blob` into *out. On failure *out is left
// in an unspecified state and the first error found is returned.
CfgDecodeError configDecode(const uint8_t* blob, size_t len, MachineCfg* out);

// Short lowercase name for status output, e.g. "dup_node".
const char* configDecodeErrorName(CfgDecodeError e);

// The slot map for `head`: [x, y, head.z, head.a] bus ids, with `none` for an
// absent node. Mirrors slotMapFor() in web/src/machine/slots.ts.
void configSlotMap(const MachineCfg& cfg, uint8_t head, uint8_t none, uint8_t out[4]);
