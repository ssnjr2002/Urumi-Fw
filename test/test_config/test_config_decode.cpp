/**
 * The Pico's config decoder (src/rp2350/config/config_decode.*) against the
 * blobs the web tests encode into web/test/fixtures/config/. A decoder that
 * drifts from the host's encoder fails here, not on the machine.
 *
 * Regenerate the fixtures:
 *   cd web && GEN_CFG_FIXTURES=1 npx vitest run test/machine/json/blob.test.ts
 */

#include "doctest.h"
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Unity build: the decoder lives under src/, which native tests do not compile.
#include "../../src/rp2350/core0/config/config_decode.cpp"

static std::vector<uint8_t> readBlob(const std::string& name) {
    static const char* prefixes[] = {"", "../", "../../", "../../../", "../../../../"};
    for (const char* p : prefixes) {
        std::ifstream f(std::string(p) + "web/test/fixtures/config/" + name, std::ios::binary);
        if (f.is_open())
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
    }
    FAIL("missing fixture " << name);
    return {};
}

TEST_CASE("good blob decodes to the test machine") {
    std::vector<uint8_t> b = readBlob("good.msgpack");
    MachineCfg c;
    REQUIRE(configDecode(b.data(), b.size(), &c) == CFG_DEC_OK);

    CHECK(c.version == CFG_SCHEMA_VERSION);
    CHECK(c.headCount == 1);
    CHECK(c.defaultHead == 0);
    CHECK(c.periphCount == 0);

    CHECK(c.x.node.id == 1);
    CHECK(c.x.node.present);
    CHECK(c.x.stepsPerUnit == doctest::Approx(160));
    CHECK(c.x.invertDir);
    CHECK(c.x.softLimits);
    CHECK(c.x.maxTravel == doctest::Approx(480));
    CHECK(c.y.node.id == 2);
    CHECK_FALSE(c.y.invertDir);
    CHECK(c.heads[0].z.node.id == 3);
    CHECK(c.heads[0].z.stepsPerUnit == doctest::Approx(1200));
    CHECK(c.heads[0].a.node.id == 4);
    CHECK(c.heads[0].a.stepsPerUnit == doctest::Approx(51.667));
    CHECK(c.heads[0].a.rotary);

    uint8_t map[4];
    configSlotMap(c, 0, 0xFF, map);
    CHECK(map[0] == 1);
    CHECK(map[1] == 2);
    CHECK(map[2] == 3);
    CHECK(map[3] == 4);
}

TEST_CASE("homing blocks decode, with cycle defaults and optional parkPos") {
    std::vector<uint8_t> b = readBlob("good.msgpack");
    MachineCfg c;
    REQUIRE(configDecode(b.data(), b.size(), &c) == CFG_DEC_OK);

    const CfgHoming& x = c.x.homing;
    REQUIRE(x.present);
    CHECK(x.cycle == 2);                 // absent: not Z
    CHECK(x.seekPositive);
    CHECK(x.seekScaler == doctest::Approx(1.2));
    CHECK(x.startFeed == doctest::Approx(2.5));
    CHECK(x.seekFeed == doctest::Approx(12.5));
    CHECK(x.latchFeed == doctest::Approx(0.78));
    CHECK(x.rampSteps == 400);
    CHECK(x.backoffDist == doctest::Approx(2));
    CHECK(x.pullOffDist == doctest::Approx(5));
    CHECK_FALSE(x.hasParkPos);

    const CfgHoming& y = c.y.homing;
    REQUIRE(y.present);
    CHECK_FALSE(y.seekPositive);
    CHECK(y.hasParkPos);
    CHECK(y.parkPos == doctest::Approx(0));

    const CfgHoming& z = c.heads[0].z.homing;
    REQUIRE(z.present);
    CHECK(z.cycle == 1);                 // absent: Z
    CHECK(z.seekPositive);

    const CfgHoming& a = c.heads[0].a.homing;
    REQUIRE(a.present);
    CHECK(a.cycle == 3);                 // explicit
    CHECK(a.budgetRevs == doctest::Approx(4));
    CHECK(a.sweepFeed == doctest::Approx(60));
    CHECK(a.toleranceDeg == doctest::Approx(2));
    CHECK(a.indexPos == doctest::Approx(90));
}

TEST_CASE("config/controller.jsonc decodes") {
    std::vector<uint8_t> b = readBlob("controller.msgpack");
    MachineCfg c;
    REQUIRE(configDecode(b.data(), b.size(), &c) == CFG_DEC_OK);
    CHECK(c.headCount == 2);
    CHECK(c.x.homing.present);
    CHECK(c.heads[1].a.homing.present);
}

TEST_CASE("frame fields decode with their defaults") {
    std::vector<uint8_t> b = readBlob("good.msgpack");
    MachineCfg c;
    REQUIRE(configDecode(b.data(), b.size(), &c) == CFG_DEC_OK);
    CHECK(c.heads[0].xOffset == doctest::Approx(0));
    CHECK(c.heads[0].yOffset == doctest::Approx(0));
    CHECK_FALSE(c.heads[0].hasProbeSwitch);
    CHECK(c.laserNode == 0);
    CHECK(c.workX == doctest::Approx(0));
    CHECK(c.workZ[0] == doctest::Approx(0));
    CHECK_FALSE(c.hasPark);
    CHECK_FALSE(c.hasLoad);

    float lo, hi;
    REQUIRE(configAxisRange(c.x, &lo, &hi));     // seeks +, no parkPos: [0, max]
    CHECK(lo == doctest::Approx(0));
    CHECK(hi == doctest::Approx(480));
    CHECK_FALSE(configAxisRange(c.heads[0].a, &lo, &hi));
}

TEST_CASE("an absent node maps to none") {
    std::vector<uint8_t> b = readBlob("good.msgpack");
    MachineCfg c;
    REQUIRE(configDecode(b.data(), b.size(), &c) == CFG_DEC_OK);
    c.heads[0].a.node.present = false;
    uint8_t map[4];
    configSlotMap(c, 0, 0xFF, map);
    CHECK(map[3] == 0xFF);
}

TEST_CASE("each bad blob is rejected for its own reason") {
    // {fixture, expected error}
    const char* cases[][2] = {
        {"version", "version"}, {"missing", "missing"}, {"steps", "steps"},
        {"node_id", "node_id"}, {"node_type", "node_type"},
        {"dup_node", "dup_node"}, {"heads", "heads"}, {"homing", "homing"},
        {"homing_kind", "homing"}, {"no_invert_dir", "missing"},
        {"frames", "frames"}, {"probe_reach", "frames"}, {"laser_node", "node_id"},
    };
    for (auto& t : cases) {
        CAPTURE(t[0]);
        std::vector<uint8_t> b = readBlob(std::string("bad_") + t[0] + ".msgpack");
        MachineCfg c;
        CHECK(std::string(configDecodeErrorName(configDecode(b.data(), b.size(), &c))) == t[1]);
    }
}

TEST_CASE("bytes that are not msgpack are rejected") {
    const uint8_t junk[] = {0xC1, 0xC1, 0xC1};
    MachineCfg c;
    CHECK(configDecode(junk, sizeof(junk), &c) == CFG_DEC_MSGPACK);
}
