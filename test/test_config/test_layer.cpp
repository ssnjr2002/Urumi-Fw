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
    c.tools[0].periodCount = 1;
    c.tools[1].id = 4;                      // revolver pen: 3 chambers
    c.tools[1].periodCount = 1;
    c.tools[1].chamberCount = 3;
    c.tools[1].cutFeed = 40;
    c.tools[1].chamberOffsets[0] = 15;
    c.tools[1].chamberOffsets[1] = 135;
    c.tools[1].chamberOffsets[2] = 375;
    c.heads[0].a.node.present = c.heads[1].a.node.present = true;
    return c;
}

static LayerHdr hdr(uint8_t tool, uint8_t head, uint8_t chamber = 0, float feed = 0, float p = 1,
                    float thickness = 1) {
    LayerHdr l;
    l.tool = tool; l.head = head; l.chamber = chamber; l.feed = feed; l.p = p;
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

TEST_CASE("a tangential tool needs its head's A") {
    MachineCfg c = twoHeads();
    c.tools[0].tangential = true;
    c.tools[0].periodCount = 2;
    c.tools[0].cornerAngleDeg = 30;
    c.tools[0].aMaxDeg = 540;
    c.tools[0].aOffsetDeg = -90;
    const uint8_t mounted[2] = { 1, 1 };
    LayerMotion mo{};
    c.heads[0].a.node.present = false;
    REQUIRE(layerResolve(c, mounted, hdr(1, 1), 10, &mo) == LAYER_OK);
    CHECK(mo.tangential);
    CHECK(mo.periodDeg == doctest::Approx(180));
    CHECK(mo.cornerAngleDeg == doctest::Approx(30));
    CHECK(mo.aMaxDeg == doctest::Approx(540));
    CHECK(mo.aOffsetDeg == doctest::Approx(-90));
    CHECK(layerResolve(c, mounted, hdr(1, 0), 10, &mo) == LAYER_BAD);   // head 0 has no A
}

TEST_CASE("a chamber layer carries its chamber's angle and needs its head's A") {
    MachineCfg c = twoHeads();
    const uint8_t mounted[2] = { 4, 1 };
    LayerMotion mo{};
    REQUIRE(layerResolve(c, mounted, hdr(4, 0, 2), 10, &mo) == LAYER_OK);
    CHECK(mo.chamber);
    CHECK(mo.chamberDeg == doctest::Approx(135));
    REQUIRE(layerResolve(c, mounted, hdr(4, 0, 3), 10, &mo) == LAYER_OK);
    CHECK(mo.chamberDeg == doctest::Approx(375));
    REQUIRE(layerResolve(c, mounted, hdr(1, 1), 10, &mo) == LAYER_OK);
    CHECK_FALSE(mo.chamber);
    c.heads[0].a.node.present = false;
    CHECK(layerResolve(c, mounted, hdr(4, 0, 1), 10, &mo) == LAYER_BAD);
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
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 0, NAN), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 0, 1, -1), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 0, 0, 0, 1, NAN), 10, &mo) == LAYER_BAD);

    const uint8_t pen[2] = { 4, 0 };
    CHECK(layerResolve(c, pen, hdr(4, 0, 0), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, pen, hdr(4, 0, 4), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, pen, hdr(4, 0, 3), 10, &mo) == LAYER_OK);
}

TEST_CASE("layer plunge: p under the top for a plunging tool, 0 for one that is not") {
    MachineCfg c = twoHeads();
    c.plungeFallback = 8;
    c.tools[0].plunge = true;               // knife
    c.tools[1].plungeFeed = 3;              // revolver pen: on the surface
    const uint8_t mounted[2] = { 4, 1 };
    LayerMotion mo;
    REQUIRE(layerResolve(c, mounted, hdr(1, 1, 0, 0, 1.5f, 2), 10, &mo) == LAYER_OK);
    CHECK(mo.top == doctest::Approx(2));
    CHECK(mo.depth == doctest::Approx(1.5f));
    CHECK(mo.plungeFeed == 8);
    REQUIRE(layerResolve(c, mounted, hdr(4, 0, 1, 0, 0, 2), 10, &mo) == LAYER_OK);   // p unused
    CHECK(mo.top == doctest::Approx(2));
    CHECK(mo.depth == 0);
    CHECK(mo.plungeFeed == 3);
}

TEST_CASE("layer plunge: p > 0 and at most maxOvercut past the material on a plunging tool") {
    MachineCfg c = twoHeads();
    c.tools[0].plunge = true;               // knife
    c.maxOvercut = 0.5f;
    const uint8_t mounted[2] = { 4, 1 };
    LayerMotion mo;
    CHECK(layerResolve(c, mounted, hdr(1, 1, 0, 0, 2.5f, 2), 10, &mo) == LAYER_OK);
    CHECK(layerResolve(c, mounted, hdr(1, 1, 0, 0, 2.6f, 2), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 1, 0, 0, 0, 2), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(1, 1, 0, 0, -1, 2), 10, &mo) == LAYER_BAD);
    CHECK(layerResolve(c, mounted, hdr(4, 0, 1, 0, 9, 2), 10, &mo) == LAYER_OK);    // pen: p unused
    CHECK(layerResolve(c, mounted, hdr(4, 0, 1, 0, NAN, 2), 10, &mo) == LAYER_BAD);
}
