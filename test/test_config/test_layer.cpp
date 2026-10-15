/**
 * layerResolve (src/rp2350/core0/ops/layer.*): a LAYER_HDR's tool on its head
 * feeds and plunge depth.
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
    c.tools[1].cutFeed = 40;
    return c;
}

static LayerHdr hdr(uint8_t tool, uint8_t head, uint8_t chamber = 0, float feed = 0, float k = 1,
                    float thickness = 0) {
    LayerHdr l;
    l.tool = tool; l.head = head; l.chamber = chamber; l.feed = feed; l.k = k;
    l.thickness = thickness;
    return l;
}

TEST_CASE("layer takes its tool on its head") {
    const MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 4, 1 };
    LayerMotion mo{};
    CHECK(layerResolve(c, mounted, hdr(1, 1, 0, 25), 10, &mo) == LAYER_OK);
    CHECK(mo.cutFeed == 25);
    mo = LayerMotion{};
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 25), 10, &mo) == LAYER_NOT_MOUNTED);
    CHECK(mo.cutFeed == 25);                       // filled for the change
    CHECK(layerResolve(c, mounted, hdr(1, 2), 10, &mo) == LAYER_BAD);   // no head 2
}

TEST_CASE("layer feed falls back to the tool's, then the machine's") {
    const MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 4, 1 };
    LayerMotion mo;
    REQUIRE(layerResolve(c, mounted, hdr(4, 0, 2), 10, &mo) == LAYER_OK);
    CHECK(mo.cutFeed == 40);
    REQUIRE(layerResolve(c, mounted, hdr(1, 1), 10, &mo) == LAYER_OK);
    CHECK(mo.cutFeed == 10);
}

TEST_CASE("layer refusals") {
    const MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 1, 0 };
    LayerMotion mo;
    CHECK(layerResolve(c, mounted, hdr(9, 0), 10, &mo) == LAYER_TOOL_UNKNOWN);
    CHECK(layerResolve(c, mounted, hdr(4, 1, 1), 10, &mo) == LAYER_NOT_MOUNTED);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 1), 10, &mo) == LAYER_BAD);   // no chambers
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, -1), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, NAN), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 0, 2.5f), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 0, 1, -1), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 0, 1, NAN), 10, &mo) == LAYER_BAD);

    const uint8_t pen[2] = { 4, 0 };
    CHECK(layerResolve(c, pen, hdr(4, 0, 0), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, pen, hdr(4, 0, 4), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, pen, hdr(4, 0, 3), 10, &mo) == LAYER_OK);
}

TEST_CASE("layer plunge: k times t for a plunging tool, 0 for one that is not") {
    MachineCfg c = twoHeads();
    c.plungeFallback = 8;
    c.tools[0].plunge = true;               // knife
    c.tools[1].plungeFeed = 3;              // revolver pen: on the surface
    const uint8_t mounted[2] = { 4, 1 };
    LayerMotion mo;
    REQUIRE(layerResolve(c, mounted, hdr(1, 1, 0, 0, 1.5f, 2), 10, &mo) == LAYER_OK);
    CHECK(mo.depth == doctest::Approx(3));
    CHECK(mo.plungeFeed == 8);
    REQUIRE(layerResolve(c, mounted, hdr(4, 0, 1, 0, 1.5f, 2), 10, &mo) == LAYER_OK);
    CHECK(mo.depth == 0);
    CHECK(mo.plungeFeed == 3);
}
