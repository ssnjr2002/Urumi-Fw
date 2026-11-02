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
// and inside it `cycle` and `parkPos`; `laser`, a head's `probeSwitch` and its `z`, and
// `positions.park` / `positions.load`; a tool's `cutFeed`, `plungeFeed` and
// `chamberOffsets`. An optional scalar's `has` flag says whether the blob held
// it; the member holds the resolved value either way.
//
// Validation is consumer-scoped: it checks what would make the Pico's own use
// of a field wrong, and leaves every other judgement to the host's
// validate.ts. Free of Arduino I/O so it builds under `pio test -e native`.
// ─────────────────────────────────────────────────────────────────────────────

#define CFG_SCHEMA_VERSION 14u  // payload version the decoder understands (blob `v`)
#define CFG_MAX_HEADS      2u
#define CFG_MAX_PERIPH     8u
#define CFG_MAX_TOOLS      8u
#define CFG_MAX_CHAMBERS   8u
#define CFG_BUS_ADDR_MAX   8u   // must equal BUS_ADDR_MAX (shared_state.h)
#define CFG_NODE_STEPPER   0x01 // NODE_TYPE_STEPPER (include/common.h)
#define CFG_NODE_KNIFE_OSC 0x03 // NODE_TYPE_KNIFE_OSC

struct CfgNode {
    uint8_t id;
    uint8_t type;
    bool    present;
};

// The homing block (docs/plans/controller-homing.md). Feeds are mm/s or deg/s.
struct CfgHoming {
    bool     present;      // false: no switch or index, the axis cannot be homed
    bool     hasCycle;
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
    float     maxTravel;  // usable length from the park position; > 0 on a linear axis
    float     jogFeed;        // jog speed, units/s, homed
    float     jogFeedUnhomed; // jog speed before homing (with jogUnhomed)
    bool      softLimits; // enforce [park, park ± maxTravel]
    bool      invertDir;  // wiring fix so + is physical
    bool      rotary;
    CfgHoming homing;
};

// A bed point, machine coordinates (docs/plans/coordinate-system.md).
struct CfgPoint {
    float x;
    float y;
};

// A tip position.
struct CfgProbeSwitch {
    float x;
    float y;
    bool  hasZ;
    float z;     // the switch's height above the bed, mm; valid if hasZ
};

struct CfgHead {
    CfgAxis  z;
    CfgAxis  a;
    float    xOffset;        // tip − anchor, signed
    float    yOffset;
    bool     hasProbeSwitch;
    CfgProbeSwitch probeSwitch;  // valid if hasProbeSwitch
    bool     hasUnlockRange;
    float    unlockRange;    // |machine A| that releases the holder, degrees; 0: no interlock
};

struct CfgPowerNode {
    uint8_t id;
    uint8_t type;
};

struct CfgDutyLimits {
    float maxOnS;
    float minOnS;
    float dwellS;
};

// A powered tool's `power` block; node id 0 on a passive tool. The node's type
// picks the command that switches it: KNIFE_OSC CMD_KNIFE_OSC, STEPPER (the
// laser standing in, ACCEPT_LASER_AS_POWER_TOOL) CMD_LASER. Duty limits are
// optional inside it.
struct CfgPower {
    CfgPowerNode  node;
    float         settleS;      // on → plunge
    bool          hasDutyLimits;
    CfgDutyLimits dutyLimits;
};

// Tip − head centre, mm.
struct CfgToolOffset {
    float xOffset;
    float yOffset;
};

// A tool profile (the blob's `toolProfiles`, keyed by name; the Pico keeps
// only the id).
struct CfgTool {
    uint8_t id;             // 1..255, unique; 0 is no tool (`mount`)
    uint8_t toolType;       // ToolType (web/src/machine/schema.ts)
    bool    tangential;     // A steers along the path
    uint8_t periodCount;    // tangential: looks the same every 360/n degrees
    float   cornerAngleDeg; // tangential: a corner turning more lifts to swivel
    bool    hasAMaxDeg;
    float   aMaxDeg;        // |A| limit either way from home; 0 = no cable
    bool    hasAOffsetDeg;
    float   aOffsetDeg;     // tangential: the edge's heading at machine A = 0 (unlock)
    bool    hasOffsetMm;
    float   offsetMm;       // tangential: the blade tip's trail behind the A axis, mm
    bool    plunge;         // cuts into the material, not on it
    CfgToolOffset toolOffset;
    bool    hasCutFeed;
    float   cutFeed;        // mm/s; 0 = the machine's
    bool    hasPlungeFeed;
    float   plungeFeed;     // mm/s; 0 = the machine's
    uint8_t chamberCount;   // 0 without chambers
    float   chamberOffsets[CFG_MAX_CHAMBERS];   // A angle per chamber, degrees
    CfgPower power;         // kind NONE on a passive tool
};

// Anchor positions. Absent park: where homing parks. Mount: where a tool
// change stops for the operator.
struct CfgPositions {
    bool     hasPark;
    CfgPoint park;
    bool     hasLoad;
    CfgPoint load;
    bool     hasMount;
    CfgPoint mount;
};

struct CfgLaser {
    uint8_t node;   // bus id the laser is wired to, 0 = none; the anchor, (0, 0)
};

// A job leaves BUFFERING once this much motion is queued, or once no record
// has arrived for this long. Seconds; jobs only.
struct CfgBuffer {
    float queueTime;
    float idleTime;
};

// Job feeds, mm/s: the cut feed when neither the layer nor its tool sets one,
// the travel to a contour's start (`feed` overrides both), and the plunge feed
// when the tool sets none.
struct CfgFeed {
    float cutFallback;
    float travelDefault;
    float plungeFallback;
    float liftFeed;       // every lift
};

// Each member has its blob field's name and nesting. The exceptions:
// `toolProfiles` is `tools`, found by id; `v` is `version`; `machine` has no
// layer here.
struct MachineCfg {
    uint16_t version;
    CfgAxis  x;
    CfgAxis  y;
    CfgHead  heads[CFG_MAX_HEADS];
    uint8_t  headCount;
    uint8_t  defaultHead;
    CfgNode  peripherals[CFG_MAX_PERIPH];
    uint8_t  periphCount;
    CfgLaser laser;
    // The work offset's config default, XY. Z is per head and set only by a
    // Z zero.
    CfgPoint work;
    CfgPositions positions;
    bool     jogUnhomed;     // relative jogs before homing, no soft limits (testing)
    bool     meshOn;         // bed mesh on at boot (the `mesh` command changes it)
    CfgBuffer buffer;
    CfgFeed  feed;
    float    zClearance;    // mm above the material top for lifts and travel
    float    maxOvercut;     // mm a zeroed tip may go below the bed (work Z 0)
    CfgTool  tools[CFG_MAX_TOOLS];
    uint8_t  toolCount;
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
    CFG_DEC_CEILING,        // a ceiling or jog feed negative or not finite; maxTravel 0 on a linear axis
    CFG_DEC_HOMING,         // a homing field out of range, or the wrong kind for the axis
    CFG_DEC_FRAMES,         // no anchor, a work Z per head missing, or a point out of reach
    CFG_DEC_BUFFER,         // a buffer time not a positive finite number
    CFG_DEC_TOOLS,          // too many tools or chambers, an id 0 or repeated, a bad number
    CFG_DEC_FEED,           // a machine feed missing or below 1 mm/s
    CFG_DEC_Z_CLEARANCE,    // zClearance negative or not finite
    CFG_DEC_OVERCUT,        // maxOvercut negative or not finite
    CFG_DEC_POWER,          // a power node not a present switchable one, or a bad settle or duty
};

// Build *out from `len` bytes at `blob`. Checks only what the struct cannot
// hold: presence, types, and counts and ids that would not fit.
CfgDecodeError configParse(const uint8_t* blob, size_t len, MachineCfg* out);

// Run every rule over `c`; the first failure is returned.
CfgDecodeError configValidate(const MachineCfg& c);

// configParse then configValidate. On failure *out is left in an unspecified
// state and the first error found is returned.
CfgDecodeError configDecode(const uint8_t* blob, size_t len, MachineCfg* out);

// Short lowercase name for status output, e.g. "dup_node".
const char* configDecodeErrorName(CfgDecodeError e);

// The slot map for `head`: [x, y, head.z, head.a] bus ids, with `none` for an
// absent node. Mirrors slotMapFor() in web/src/machine/slots.ts.
void configSlotMap(const MachineCfg& cfg, uint8_t head, uint8_t none, uint8_t out[4]);

// The tool with `id`, or nullptr.
const CfgTool* configTool(const MachineCfg& cfg, uint8_t id);

// Where a linear home leaves the axis, in units: parkPos, or the default
// frame (the trip at 0 homing −, at maxTravel + pullOffDist homing +).
float configParkPos(const CfgAxis& a);

// The anchor's soft range on a homeable linear axis: [park, park ± maxTravel],
// away from the switch. False for an axis with no linear home.
bool configAxisRange(const CfgAxis& a, float* lo, float* hi);
