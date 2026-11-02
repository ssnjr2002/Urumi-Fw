/**
 * Runtime config edits (src/rp2350/core0/config/runtime_edit.*) against the
 * host's encoded test machine.
 */

#include "doctest.h"
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// Unity build: runtime_edit lives under src/, which native tests do not compile.
// test_config_decode.cpp builds the decoder it links against.
#include "../../src/rp2350/core0/config/runtime_edit.cpp"

static std::vector<uint8_t> goodBlob() {
    static const char* prefixes[] = {"", "../", "../../", "../../../", "../../../../"};
    for (const char* p : prefixes) {
        std::ifstream f(std::string(p) + "web/test/fixtures/config/good.msgpack", std::ios::binary);
        if (f.is_open())
            return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
    }
    FAIL("missing fixture");
    return {};
}

static MachineCfg goodCfg() {
    const std::vector<uint8_t> b = goodBlob();
    MachineCfg c;
    REQUIRE(configDecode(b.data(), b.size(), &c) == CFG_DEC_OK);
    return c;
}

static RtError set(MachineCfg& c, const std::string& path, const char* text, RtApplied* a = nullptr) {
    RtApplied local;
    return runtimeSet(c, path.c_str(), path.size(), text, a ? a : &local);
}

static std::string get(const MachineCfg& c, const std::string& path) {
    char out[64];
    return runtimeGet(c, path.c_str(), path.size(), out, sizeof out) ? out : "!";
}

// Encode into a buffer holding the stored blob, as the Pico's staging buffer does.
static RtEncodeError encode(const MachineCfg& c, std::vector<uint8_t>& out, MachineCfg* check) {
    std::vector<uint8_t> buf = goodBlob();
    const size_t len = buf.size();
    buf.resize(32768);
    size_t n = 0;
    const RtEncodeError e = runtimeEncode(c, buf.data(), len, buf.size(), check, &n);
    buf.resize(n);
    out = buf;
    return e;
}

TEST_CASE("set writes a field and reports its effects") {
    MachineCfg c = goodCfg();
    RtApplied a;

    CHECK(set(c, "machine.x.maxFeed", "70", &a) == RT_OK);
    CHECK(c.x.maxFeed == 70.0f);
    CHECK(a.changed);
    CHECK(a.effects == 0);

    CHECK(set(c, "machine.heads.0.z.stepsPerUnit", "1000", &a) == RT_OK);
    CHECK(c.heads[0].z.stepsPerUnit == 1000.0f);
    CHECK((a.effects & RT_FX_voidDatum) != 0);
    CHECK(a.axis == RT_AXIS_Z);
    CHECK(a.head == 0);
    CHECK(a.node == c.heads[0].z.node.id);

    CHECK(set(c, "machine.x.invertDir", "false", &a) == RT_OK);
    CHECK_FALSE(c.x.invertDir);
    CHECK(a.axis == RT_AXIS_X);

    CHECK(set(c, "machine.buffer.queueTime", "2.5", &a) == RT_OK);
    CHECK((a.effects & RT_FX_republishBuffer) != 0);
    CHECK(set(c, "machine.meshOn", "true", &a) == RT_OK);
    CHECK(c.meshOn);
    CHECK((a.effects & RT_FX_applyMesh) != 0);
    CHECK(set(c, "machine.x.homing.rampSteps", "12", &a) == RT_OK);
    CHECK(c.x.homing.rampSteps == 12);
}

TEST_CASE("the value already held is no change") {
    MachineCfg c = goodCfg();
    RtApplied a;
    const std::string held = get(c, "machine.x.stepsPerUnit");
    CHECK(set(c, "machine.x.stepsPerUnit", held.c_str(), &a) == RT_OK);
    CHECK_FALSE(a.changed);
    CHECK(a.effects == 0);
}

TEST_CASE("a tool is addressed by name") {
    MachineCfg c = goodCfg();
    const std::string tool = std::string("toolProfiles.") + c.tools[0].name + ".";
    CHECK(set(c, tool + "cutFeed", "25") == RT_OK);
    CHECK(c.tools[0].cutFeed == 25.0f);
    CHECK(c.tools[0].hasCutFeed);
    CHECK(set(c, "toolProfiles.nosuchtool.cutFeed", "25") == RT_NO_FIELD);
}

TEST_CASE("paths without a row are refused") {
    MachineCfg c = goodCfg();
    for (const char* p : {"machine.x.node.id", "machine.x.softLimits", "machine.defaultHead", "machine.heads.7.xOffset",
                          "machine.nope", "nope", "machine.x", "toolProfiles", "machine.x.maxfeed"})
        CHECK(set(c, p, "1") == RT_NO_FIELD);
}

TEST_CASE("a homing row needs that homing kind") {
    MachineCfg c = goodCfg();
    REQUIRE_FALSE(c.x.rotary);
    REQUIRE(c.heads[0].a.rotary);
    CHECK(set(c, "machine.x.homing.sweepFeed", "5") == RT_NO_FIELD);
    CHECK(set(c, "machine.heads.0.a.homing.seekFeed", "5") == RT_NO_FIELD);
    c.x.homing.present = false;
    CHECK(set(c, "machine.x.homing.startFeed", "5") == RT_NO_FIELD);
}

TEST_CASE("bad values leave the config as it was") {
    MachineCfg c = goodCfg();
    const float feed = c.x.maxFeed;
    CHECK(set(c, "machine.x.maxFeed", "fast") == RT_BAD_VALUE);
    CHECK(set(c, "machine.x.maxFeed", "1 2") == RT_BAD_VALUE);
    CHECK(set(c, "machine.x.maxFeed", "nan") == RT_BAD_VALUE);
    CHECK(set(c, "machine.x.maxFeed", "inf") == RT_BAD_VALUE);
    CHECK(set(c, "machine.x.maxFeed", "-1") == RT_RANGE);
    CHECK(set(c, "machine.x.maxFeed", "0.123456789") == RT_PRECISION);
    CHECK(set(c, "machine.x.stepsPerUnit", "0") == RT_RANGE);
    CHECK(set(c, "machine.x.invertDir", "1") == RT_BAD_VALUE);
    CHECK(set(c, "machine.x.homing.rampSteps", "1.5") == RT_BAD_VALUE);
    CHECK(set(c, "machine.x.homing.rampSteps", "70000") == RT_RANGE);
    CHECK(set(c, "machine.positions.park", "10") == RT_BAD_VALUE);
    CHECK(c.x.maxFeed == feed);
}

TEST_CASE("a rule's refusal restores the field and its flag") {
    MachineCfg c = goodCfg();
    RtApplied a;
    const float latch = c.x.homing.latchFeed;
    const std::string seek = get(c, "machine.x.homing.seekFeed");
    CHECK(set(c, "machine.x.homing.latchFeed", seek.c_str(), &a) == RT_RULE);
    CHECK(a.rule == CFG_DEC_HOMING);
    CHECK_FALSE(a.changed);
    CHECK(c.x.homing.latchFeed == latch);

    c.positions.hasLoad = false;
    c.positions.load = CfgPoint{1, 2};
    CHECK(set(c, "machine.positions.load", "-5000 0") == RT_RULE);
    CHECK_FALSE(c.positions.hasLoad);
    CHECK(c.positions.load.x == 1.0f);
}

TEST_CASE("the homing rules hold at load and on set") {
    MachineCfg c = goodCfg();
    c.x.homing.seekFeed = c.x.maxFeed + 1;
    CHECK(configValidate(c) == CFG_DEC_HOMING);
    c.x.maxFeed = 0;   // uncapped
    CHECK(configValidate(c) == CFG_DEC_OK);

    c = goodCfg();
    c.x.homing.latchFeed = c.x.homing.seekFeed;
    CHECK(configValidate(c) == CFG_DEC_HOMING);

    c = goodCfg();
    c.heads[0].a.homing.sweepFeed = c.heads[0].a.maxFeed + 1;
    CHECK(configValidate(c) == CFG_DEC_HOMING);
}

TEST_CASE("a point is set whole, and sets its flag") {
    MachineCfg c = goodCfg();
    c.positions.hasLoad = false;
    CHECK(set(c, "machine.positions.load", "10 20") == RT_OK);
    CHECK(c.positions.hasLoad);
    CHECK(c.positions.load.x == 10.0f);
    CHECK(c.positions.load.y == 20.0f);
    CHECK(get(c, "machine.positions.load") == "10,20");
}

TEST_CASE("a switch height needs its switch") {
    MachineCfg c = goodCfg();
    c.heads[0].hasProbeSwitch = false;
    c.heads[0].probeSwitch.hasZ = false;
    CHECK(set(c, "machine.heads.0.probeSwitch.z", "5") == RT_ABSENT);
    CHECK(set(c, "machine.heads.0.probeSwitch", "10 20") == RT_OK);
    CHECK_FALSE(c.heads[0].probeSwitch.hasZ);
    CHECK(set(c, "machine.heads.0.probeSwitch.z", "5") == RT_OK);
    CHECK(c.heads[0].probeSwitch.z == 5.0f);
    CHECK(c.heads[0].probeSwitch.x == 10.0f);
}

TEST_CASE("get formats the shortest decimal, - for absent, unknown for no field") {
    MachineCfg c = goodCfg();
    CHECK(set(c, "machine.buffer.idleTime", "0.1") == RT_OK);
    CHECK(get(c, "machine.buffer.idleTime") == "0.1");
    CHECK(get(c, "machine.x.maxTravel") == "480");
    c.positions.hasLoad = false;
    CHECK(get(c, "machine.positions.load") == "-");
    CHECK(get(c, "machine.meshOn") == (c.meshOn ? "true" : "false"));
    CHECK(get(c, "machine.nope") == "!");
}

TEST_CASE("the shortest decimal is the one typed") {
    CHECK(runtimeShortest(0.1f) == 0.1);
    CHECK(runtimeShortest(51.667f) == 51.667);
    CHECK(runtimeShortest(480.0f) == 480.0);
    CHECK(runtimeShortest(1e-3f) == 1e-3);
}

TEST_CASE("encoding an unedited config keeps every value") {
    const MachineCfg c = goodCfg();
    std::vector<uint8_t> out;
    MachineCfg check;
    REQUIRE(encode(c, out, &check) == RT_ENC_OK);

    const std::vector<uint8_t> in = goodBlob();
    JsonDocument before, after;
    REQUIRE_FALSE(deserializeMsgPack(before, in.data(), in.size(), DeserializationOption::NestingLimit(16)));
    REQUIRE_FALSE(deserializeMsgPack(after, out.data(), out.size(), DeserializationOption::NestingLimit(16)));
    CHECK(before == after);
}

TEST_CASE("encoding writes edits and leaves the rest") {
    MachineCfg c = goodCfg();
    CHECK(set(c, "machine.x.maxFeed", "70") == RT_OK);
    CHECK(set(c, "machine.buffer.idleTime", "0.1") == RT_OK);
    CHECK(set(c, "machine.positions.load", "10 20") == RT_OK);
    std::vector<uint8_t> out;
    MachineCfg check;
    REQUIRE(encode(c, out, &check) == RT_ENC_OK);

    JsonDocument doc;
    REQUIRE_FALSE(deserializeMsgPack(doc, out.data(), out.size(), DeserializationOption::NestingLimit(16)));
    CHECK(doc["machine"]["x"]["maxFeed"].is<int>());
    CHECK(doc["machine"]["x"]["maxFeed"].as<int>() == 70);
    CHECK(doc["machine"]["buffer"]["idleTime"].as<double>() == 0.1);   // the host's double
    CHECK(doc["machine"]["positions"]["load"]["x"].as<int>() == 10);

    const std::vector<uint8_t> in = goodBlob();
    JsonDocument before;
    REQUIRE_FALSE(deserializeMsgPack(before, in.data(), in.size(), DeserializationOption::NestingLimit(16)));
    CHECK(doc["machine"]["x"]["node"] == before["machine"]["x"]["node"]);
    CHECK(doc["v"] == before["v"]);
}

TEST_CASE("an absent optional field stays absent") {
    MachineCfg c = goodCfg();
    c.positions.hasLoad = false;
    std::vector<uint8_t> out;
    MachineCfg check;
    REQUIRE(encode(c, out, &check) == RT_ENC_OK);
    CHECK_FALSE(check.positions.hasLoad);
    JsonDocument doc;
    REQUIRE_FALSE(deserializeMsgPack(doc, out.data(), out.size(), DeserializationOption::NestingLimit(16)));
    CHECK_FALSE(doc["machine"]["positions"]["load"].is<JsonObject>());
}

TEST_CASE("encoding refuses a blob that will not fit") {
    const MachineCfg c = goodCfg();
    std::vector<uint8_t> buf = goodBlob();
    MachineCfg check;
    size_t n;
    CHECK(runtimeEncode(c, buf.data(), buf.size(), 16, &check, &n) == RT_ENC_TOO_BIG);
}

TEST_CASE("encoding refuses a stored blob that is not msgpack") {
    const MachineCfg c = goodCfg();
    uint8_t junk[4] = {0xC1, 0xC1, 0xC1, 0xC1};
    MachineCfg check;
    size_t n;
    CHECK(runtimeEncode(c, junk, sizeof junk, sizeof junk, &check, &n) == RT_ENC_DECODE);
}
