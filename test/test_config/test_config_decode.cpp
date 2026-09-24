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
#include "../../src/rp2350/config/config_decode.cpp"

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
    CHECK(c.x.invert);
    CHECK(c.y.node.id == 2);
    CHECK_FALSE(c.y.invert);
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
    const char* reasons[] = {"version", "missing", "steps", "node_id",
                             "node_type", "dup_node", "heads"};
    for (const char* r : reasons) {
        CAPTURE(r);
        std::vector<uint8_t> b = readBlob(std::string("bad_") + r + ".msgpack");
        MachineCfg c;
        CHECK(std::string(configDecodeErrorName(configDecode(b.data(), b.size(), &c))) == r);
    }
}

TEST_CASE("bytes that are not msgpack are rejected") {
    const uint8_t junk[] = {0xC1, 0xC1, 0xC1};
    MachineCfg c;
    CHECK(configDecode(junk, sizeof(junk), &c) == CFG_DEC_MSGPACK);
}
