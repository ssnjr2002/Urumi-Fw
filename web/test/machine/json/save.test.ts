/**
 * Tests for save.ts — modelToFile is the reverse of loadConfig.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { readFixture, readFixtureBytes } from "../../helpers.js";
import { loadConfig } from "../../../src/machine/json/load.js";
import { stripJsonComments } from "../../../src/machine/json/image.js";
import { decodeConfigBlob } from "../../../src/machine/json/blob.js";
import { modelToFile } from "../../../src/machine/json/save.js";
import type { PipelineConfig } from "../../../src/machine/schema.js";

function load(text: string): PipelineConfig {
    const r = loadConfig(text);
    if (!r.ok) throw new Error(r.errors.join("; "));
    return r.config;
}

const sources: [string, () => PipelineConfig][] = [
    ["config/controller.jsonc", () => load(stripJsonComments(
        readFileSync(join(__dirname, "../../../../config/controller.jsonc"), "utf8")))],
    ["test-machine.json", () => load(readFixture("test-machine.json"))],
    ["a pulled controller.msgpack", () => decodeConfigBlob(readFixtureBytes("config/controller.msgpack"))],
];

describe("modelToFile", () => {
    it.each(sources)("%s loads back to the same model", (_, model) => {
        const m = model();
        expect(load(modelToFile(m))).toEqual(m);
    });

    it("writes tools by preset name, without the derived fields", () => {
        const file = JSON.parse(modelToFile(load(readFixture("test-machine.json"))));
        expect(file.toolProfiles).toBeUndefined();
        for (const tool of Object.values<Record<string, unknown>>(file.tools)) {
            expect(tool).not.toHaveProperty("name");
            expect(tool).not.toHaveProperty("toolType");
            expect(tool).not.toHaveProperty("requiredPeripheralTypes");
        }
        expect(file.machine.heads[0].accepts.every((a: unknown) => typeof a === "string")).toBe(true);
    });
});
