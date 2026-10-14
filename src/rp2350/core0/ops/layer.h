#pragma once
#include <stdint.h>
#include "../config/config_decode.h"

// layer — what a LAYER_HDR asks for (usb_protocol.h): the head that carries
// its tool, and the cut feed. Pure; the data plane applies it.

enum LayerResult : uint8_t {
    LAYER_OK,
    LAYER_BAD,            // chamber, feed or k out of range
    LAYER_TOOL_UNKNOWN,   // the id is not in the config
    LAYER_NOT_MOUNTED,    // no head has the tool mounted
};

struct LayerHdr {
    uint8_t tool, chamber;
    float   feed, k;
};

// `mounted[h]` is head h's tool id (tools.h); the lowest head with the tool
// wins. `machineFeed` is the cut feed when neither the header nor the tool
// sets one. Fills *head and *cutFeed on LAYER_OK.
LayerResult layerResolve(const MachineCfg& c, const uint8_t* mounted,
                         const LayerHdr& l, float machineFeed,
                         uint8_t* head, float* cutFeed);
