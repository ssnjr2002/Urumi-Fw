/**
 * layerResolve (src/rp2350/core0/ops/layer.*): a LAYER_HDR's head and cut feed.
 * configTool comes from test_config_decode.cpp's unity build of the decoder.
 */

#include "doctest.h"
#include <math.h>
#include "../../src/rp2350/core0/ops/layer.cpp"

static MachineCfg twoHeads() {
    MachineCfg c{};
    c.headCount = 2;
    c.toolCount = 2;
    c.tools[0].id = 1;                      // knife: no chambers, no feed
    c.tools[1].id = 4;                      // revolver pen: 3 chambers
    c.tools[1].chamberCount = 3;
    c.tools[1].pathFeed = 40;
    return c;
}

static LayerHdr hdr(uint8_t tool, uint8_t chamber = 0, float feed = 0, float k = 1) {
    LayerHdr l;
    l.tool = tool; l.chamber = chamber; l.feed = feed; l.k = k;
    return l;
}

TEST_CASE("layer picks the head with the tool mounted") {
    const MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 4, 1 };
    uint8_t head = 0xFF;
    float cut = 0;
    CHECK(layerResolve(c, mounted, hdr(1, 0, 25), 10, &head, &cut) == LAYER_OK);
    CHECK(head == 1);
    CHECK(cut == 25);

    const uint8_t both[2] = { 1, 1 };
    CHECK(layerResolve(c, both, hdr(1), 10, &head, &cut) == LAYER_OK);
    CHECK(head == 0);                       // the lowest head wins
}

TEST_CASE("layer feed falls back to the tool's, then the machine's") {
    const MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 4, 1 };
    uint8_t head;
    float cut;
    REQUIRE(layerResolve(c, mounted, hdr(4, 2), 10, &head, &cut) == LAYER_OK);
    CHECK(cut == 40);
    REQUIRE(layerResolve(c, mounted, hdr(1), 10, &head, &cut) == LAYER_OK);
    CHECK(cut == 10);
}

TEST_CASE("layer refusals") {
    const MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 1, 0 };
    uint8_t head;
    float cut;
    CHECK(layerResolve(c, mounted, hdr(9), 10, &head, &cut) == LAYER_TOOL_UNKNOWN);
    CHECK(layerResolve(c, mounted, hdr(4, 1), 10, &head, &cut) == LAYER_NOT_MOUNTED);
    CHECK(layerResolve(c, mounted, hdr(1, 1), 10, &head, &cut) == LAYER_BAD);   // no chambers
    CHECK(layerResolve(c, mounted, hdr(1, 0, -1), 10, &head, &cut) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, NAN), 10, &head, &cut) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 2.5f), 10, &head, &cut) == LAYER_BAD);

    const uint8_t pen[2] = { 4, 0 };
    CHECK(layerResolve(c, pen, hdr(4, 0), 10, &head, &cut) == LAYER_BAD);
    CHECK(layerResolve(c, pen, hdr(4, 4), 10, &head, &cut) == LAYER_BAD);
    CHECK(layerResolve(c, pen, hdr(4, 3), 10, &head, &cut) == LAYER_OK);
}
