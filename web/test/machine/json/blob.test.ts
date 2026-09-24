/**
 * Tests for blob.ts — PipelineConfig ↔ the msgpack blob stored on the Pico.
 *
 * Also owns the shared fixture set in test/fixtures/config/: one good blob and
 * one bad blob per Pico rejection, read by the firmware's decoder test
 * (test/test_config/test_config_decode.cpp). The committed files must equal
 * what this encoder produces; regenerate them with
 *
 *   GEN_CFG_FIXTURES=1 npx vitest run test/machine/json/blob.test.ts
 */

import { describe, it, expect } from "vitest";
import { existsSync, mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";
import { encode } from "@msgpack/msgpack";
import { FIXTURES, readFixture, readFixtureBytes } from "../../helpers.js";
import { loadConfig } from "../../../src/machine/json/load.js";
import { validateConfig } from "../../../src/machine/json/validate.js";
import {
    CONFIG_BLOB_VERSION,
    decodeConfigBlob,
    encodeConfigBlob,
} from "../../../src/machine/json/blob.js";
import type { PipelineConfig } from "../../../src/machine/schema.js";

function testMachine(): PipelineConfig {
    const r = loadConfig(readFixture("test-machine.json"));
    if (!r.ok) throw new Error(r.errors.join("; "));
    return r.config;
}

/** A plain mutable copy with undefined fields dropped, as msgpack sees it. */
function plain(config: PipelineConfig): Record<string, any> {
    return JSON.parse(JSON.stringify(config));
}

describe("config blob", () => {
    it("round-trips a resolved config", () => {
        const config = testMachine();
        expect(decodeConfigBlob(encodeConfigBlob(config))).toEqual(plain(config));
    });

    it("carries the schema version", () => {
        const bytes = encode({ ...plain(testMachine()), v: CONFIG_BLOB_VERSION + 1 });
        expect(() => decodeConfigBlob(bytes)).toThrow(/unsupported version/);
    });

    it("rejects a blob that is not a map", () => {
        expect(() => decodeConfigBlob(encode([1, 2, 3]))).toThrow(/not a map/);
    });
});

// ── shared fixtures ───────────────────────────────────────────────────────────

type Mutate = (c: Record<string, any>) => void;

/**
 * Bad blobs, one per Pico rejection. The name is what the firmware test
 * expects configDecodeErrorName() to report. `host` marks the ones the host's
 * validateConfig must also reject — the rest are structural and only
 * reachable by a blob the host would never encode.
 */
const BAD: readonly { name: string; host: boolean; mutate: Mutate; v?: number }[] = [
    { name: "version", host: false, mutate: () => {}, v: CONFIG_BLOB_VERSION + 1 },
    { name: "missing", host: false, mutate: (c) => delete c.machine.x.stepsPerUnit },
    { name: "steps", host: false, mutate: (c) => (c.machine.heads[0].z.stepsPerUnit = 0) },
    { name: "node_id", host: false, mutate: (c) => (c.machine.y.node.id = 9) },
    { name: "node_type", host: false, mutate: (c) => (c.machine.heads[0].a.node.type = 2) },
    { name: "dup_node", host: true, mutate: (c) => (c.machine.y.node.id = c.machine.x.node.id) },
    { name: "heads", host: true, mutate: (c) => (c.machine.defaultHead = 3) },
];

const DIR = join(FIXTURES, "config");
const GEN = process.env.GEN_CFG_FIXTURES === "1";

function badBlob(b: (typeof BAD)[number]): { config: Record<string, any>; bytes: Uint8Array } {
    const config = plain(testMachine());
    b.mutate(config);
    return { config, bytes: encode({ v: b.v ?? CONFIG_BLOB_VERSION, ...config }) };
}

function checkFixture(file: string, bytes: Uint8Array): void {
    const path = join(DIR, file);
    if (GEN) {
        if (!existsSync(DIR)) mkdirSync(DIR, { recursive: true });
        writeFileSync(path, bytes);
        return;
    }
    expect(readFixtureBytes(join("config", file))).toEqual(bytes);
}

describe("config fixtures (shared with the Pico decoder test)", () => {
    it("good.msgpack is the encoded test machine", () => {
        checkFixture("good.msgpack", encodeConfigBlob(testMachine()));
    });

    for (const b of BAD) {
        it(`bad_${b.name}.msgpack`, () => {
            const { config, bytes } = badBlob(b);
            checkFixture(`bad_${b.name}.msgpack`, bytes);
            if (b.host) {
                expect(validateConfig(config as PipelineConfig).errors).not.toEqual([]);
            }
        });
    }
});
