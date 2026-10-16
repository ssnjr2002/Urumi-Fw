#pragma once
#include <stdint.h>
#include "../config/config_decode.h"

// layer — what a LAYER_HDR asks for (usb_protocol.h): its tool on its head, the
// cut and plunge feeds, and the plunge depth. Pure; the data plane applies it.

enum LayerResult : uint8_t {
    LAYER_OK,
    LAYER_BAD,            // head, chamber, feed, k or thickness out of range
    LAYER_TOOL_UNKNOWN,   // the id is not in the config
    LAYER_NOT_MOUNTED,    // the head has another tool mounted, or none
};

struct LayerHdr {
    uint8_t tool, head, chamber;
    float   feed, k;
    float   thickness;    // mm
};

struct LayerMotion {
    float cutFeed, plungeFeed;
    float depth;          // mm below work Z; 0 for a tool that does not plunge
    uint8_t tool;
    CfgPower power;       // node 0 on a passive tool
};

// `mounted[h]` is head h's tool id (tools.h). `machineFeed` is the cut feed
// when neither the header nor the tool sets one. Fills *out on LAYER_OK and
// LAYER_NOT_MOUNTED.
LayerResult layerResolve(const MachineCfg& c, const uint8_t* mounted,
                         const LayerHdr& l, float machineFeed, LayerMotion* out);
