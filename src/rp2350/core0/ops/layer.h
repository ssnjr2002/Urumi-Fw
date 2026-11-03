#pragma once
#include <stdint.h>
#include "../config/config_decode.h"

// layer — what a LAYER_HDR and its TOOL_HDR ask for (usb_protocol.h): the tool on
// its head, the cut and plunge feeds, and the heights; and the origins a tool's
// bbox allows. Pure; the data plane applies them.

enum LayerResult : uint8_t {
    LAYER_OK,
    LAYER_BAD,            // head or chamber out of range, a plunge past
                          // maxOvercut below the bed, or a tangential or chamber
                          // tool on a head without A
    LAYER_TOOL_UNKNOWN,   // the id is not in the config
    LAYER_NOT_MOUNTED,    // the head has another tool mounted, or none
};

struct LayerHdr {
    uint8_t tool, head, chamber;
    float   feed;
    float   p;            // plunge depth below the material top, mm
    float   thickness;    // mm
};

struct LayerMotion {
    float cutFeed, plungeFeed;
    float top;            // the material top, mm above work Z
    float depth;          // mm below the top; 0 for a tool that does not plunge
    uint8_t tool;
    CfgPower power;       // node 0 on a passive tool
    bool  tangential;     // A swivels to the path heading
    float periodDeg;      // tangential: 360 / periodCount
    float cornerAngleDeg; // tangential: a corner turning more lifts to swivel
    float aMaxDeg;        // 0 = no cable
    float aOffsetDeg;     // tangential: edge heading at machine A = 0
    bool  chamber;        // A holds chamberDeg through the layer
    float chamberDeg;     // the chamber's machine A, any equivalent
};

// What needs no tool: feed, p and thickness finite and in range.
bool layerValid(const LayerHdr& l);

// The head exists and has the A this tool needs.
bool layerHeadFits(const MachineCfg& c, const CfgTool& t, uint8_t head);

// `l` passed layerValid. `mounted[h]` is head h's tool id (tools.h).
// `machineFeed` is the cut feed when neither the header nor the tool sets one.
// Fills *out on LAYER_OK and LAYER_NOT_MOUNTED.
LayerResult layerResolve(const MachineCfg& c, const uint8_t* mounted,
                         const LayerHdr& l, float machineFeed, LayerMotion* out);

// One axis's valid work offsets for a tip `d` from the anchor, its blade `blade`
// mm off the axis: what keeps [bmin, bmax] inside the soft range [lo, hi].
struct OriginRange {
    float lo, hi;
};
OriginRange layerOrigins(float lo, float hi, float bmin, float bmax, float d, float blade);

// Narrow `r` by `by`. False when nothing is left; *high then says the high bound
// was the one that closed it.
bool layerOriginsNarrow(OriginRange* r, const OriginRange& by, bool* high);
