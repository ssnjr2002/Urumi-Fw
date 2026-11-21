import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { readFixture } from "../../helpers.js";
import { crc32 } from "../../../src/packets/crc.js";
import { CFG_MAX_BYTES } from "../../../src/packets/cfg.js";
import { decodeConfig } from "../../../src/config/blob.js";
import {
    CFG_HEADER_BYTES,
    wrapConfigBin,
    configBin,
} from "../../../src/config/image.js";
import { parseConfig, stripJsonComments } from "../../../src/config/load.js";

describe("stripJsonComments", () => {
    it("drops line and block comments", () => {
        const s = stripJsonComments('{ // a\n "x": /* b */ 1 }');
        expect(JSON.parse(s)).toEqual({ x: 1 });
    });

    it("keeps comment markers inside strings, escaped quotes included", () => {
        const text = '{ "u": "http://a/*b*/", "q": "say \\"//\\"" }';
        expect(JSON.parse(stripJsonComments(text))).toEqual(JSON.parse(text));
    });

    it("keeps line numbers", () => {
        const text = "{\n/* one\ntwo */\n\"x\": 1 // end\n}";
        expect(stripJsonComments(text).split("\n")).toHaveLength(text.split("\n").length);
    });
});

describe("wrapConfigBin", () => {
    // config_store.cpp fileValid: version 1, length 1..CFG_MAX_BYTES,
    // size = header + length, CRC32 over the payload.
    it("writes the header fileValid checks", () => {
        const payload = new Uint8Array([1, 2, 3, 4, 5]);
        const img = wrapConfigBin(payload, 7);
        const dv = new DataView(img.buffer);
        expect(dv.getUint16(0, true)).toBe(1);
        expect(dv.getUint32(4, true)).toBe(7);
        expect(dv.getUint32(8, true)).toBe(payload.length);
        expect(dv.getUint32(12, true)).toBe(crc32(payload) >>> 0);
        expect(img.length).toBe(CFG_HEADER_BYTES + payload.length);
        expect(img.subarray(CFG_HEADER_BYTES)).toEqual(payload);
    });

    it("refuses an empty or oversized payload", () => {
        expect(() => wrapConfigBin(new Uint8Array(0))).toThrow();
        expect(() => wrapConfigBin(new Uint8Array(CFG_MAX_BYTES + 1))).toThrow();
    });
});

describe("configBin", () => {
    it("carries the encoded config", () => {
        const img = configBin(readFixture("test-machine.json"));
        const config = decodeConfig(img.subarray(CFG_HEADER_BYTES));
        expect(config.machine.x.invertDir).toBe(true);
    });

    it("lists the loader's errors", () => {
        expect(() => configBin('{ "machine": {} }')).toThrow(/config invalid/);
    });

    it("refuses a config that parses but fails validation", () => {
        const json = JSON.parse(readFixture("test-machine.json")) as { machine: { slew?: object } };
        json.machine.slew = { feed: -1 };
        const text = JSON.stringify(json);
        expect(parseConfig(text).ok).toBe(true);
        expect(() => configBin(text)).toThrow(/machine\.slew\.feed: must be >= 0/);
    });

    it("builds config/controller.jsonc", () => {
        const text = readFileSync(join(__dirname, "../../../../config/controller.jsonc"), "utf8");
        expect(configBin(text).length).toBeGreaterThan(CFG_HEADER_BYTES);
    });
});
