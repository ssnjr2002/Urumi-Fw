/**
 * Tests for save.ts — saveConfig is the reverse of loadConfig.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { readFixture, readFixtureBytes } from "../../helpers.js";
import { loadConfig } from "../../../src/config/load.js";
import { decodeConfig } from "../../../src/config/blob.js";
import { saveConfig } from "../../../src/config/save.js";
import type { ControllerConfig } from "../../../src/config/schema.js";

function load(text: string): ControllerConfig {
    const r = loadConfig(text);
    if (!r.ok) throw new Error(r.errors.join("; "));
    return r.config;
}

const sources: [string, () => ControllerConfig][] = [
    ["config/controller.jsonc", () => load(
        readFileSync(join(__dirname, "../../../../config/controller.jsonc"), "utf8"))],
    ["test-machine.json", () => load(readFixture("test-machine.json"))],
    ["a pulled controller.msgpack", () => decodeConfig(readFixtureBytes("config/controller.msgpack"))],
];

describe("saveConfig", () => {
    it.each(sources)("%s loads back to the same model", (_, model) => {
        const m = model();
        expect(load(saveConfig(m))).toEqual(m);
    });

    it("writes tools by preset name, without the derived fields", () => {
        const file = JSON.parse(saveConfig(load(readFixture("test-machine.json"))));
        expect(file.toolProfiles).toBeUndefined();
        for (const tool of Object.values<Record<string, unknown>>(file.tools)) {
            expect(tool).not.toHaveProperty("name");
            expect(tool).not.toHaveProperty("toolType");
            expect(tool).not.toHaveProperty("requiredPeripheralTypes");
        }
        expect(file.machine.heads[0].accepts.every((a: unknown) => typeof a === "string")).toBe(true);
    });
});
