/**
 * Tests for configLoader.ts — JSON → PipelineConfig parser.
 * Verifies required-field checking, optional defaults, tool preset
 * patching, quality overrides, and error reporting.
 */

import { describe, it, expect } from "vitest";
import { readFixture } from "../helpers.js";

import { parseConfig } from "../../src/config/configLoader.js";
import {
    KNIFE,
    PEN,
    REVOLVER_PEN,
    defaultConfig,
    qualityConfig,
} from "../../src/config/config.js";


function readJson(name: string): string {
    return readFixture(name);
}

const TEST_MACHINE = readJson("test-machine.json");

// ── valid config ──────────────────────────────────────────────────────────────

describe("configLoader: valid config", () => {
    it("parses test-machine.json successfully", () => {
        const result = parseConfig(TEST_MACHINE);
        expect(result.ok).toBe(true);
    });

    it("machine.fCpu = 150000000", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.fCpu).toBe(150_000_000);
    });

    it("X: 160 steps/mm, node 1, invert, maxRate 80, accel 1000", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        const x = r.config.machine.x;
        expect(x.stepsPerUnit).toBe(160);
        expect(x.node.id).toBe(1);
        expect(x.invert).toBe(true);
        expect(x.maxFeed).toBe(80);
        expect(x.maxAccel).toBe(1000);
    });

    it("Y: 160 steps/mm, node 2, no invert", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        const y = r.config.machine.y;
        expect(y.stepsPerUnit).toBe(160);
        expect(y.node.id).toBe(2);
        expect(y.invert).toBe(false);
    });

    it("Z: 1200 steps/mm, node 3, invert, maxRate 10", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        const z = r.config.machine.heads[0]!.z;
        expect(z.stepsPerUnit).toBe(1200);
        expect(z.node.id).toBe(3);
        expect(z.invert).toBe(true);
        expect(z.maxFeed).toBe(10);
    });

    it("A: 51.667 steps/deg, node 4, rotary, invert, maxRate 100, accel 2000", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        const a = r.config.machine.heads[0]!.a;
        expect(a.stepsPerUnit).toBeCloseTo(51.667, 3);
        expect(a.node.id).toBe(4);
        expect(a.rotary).toBe(true);
        expect(a.invert).toBe(true);
        expect(a.maxFeed).toBe(100);
        expect(a.maxAccel).toBe(2000);
    });

    it("head has KNIFE profile (resolved from tool name)", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.heads[0]!.profile).toBe(KNIFE);
    });

    it("matches defaultConfig().machine (same calibration values)", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        const dm = defaultConfig().machine;
        const pm = r.config.machine;
        expect(pm.fCpu).toBe(dm.fCpu);
        expect(pm.x.stepsPerUnit).toBe(dm.x.stepsPerUnit);
        expect(pm.x.invert).toBe(dm.x.invert);
        expect(pm.x.maxFeed).toBe(dm.x.maxFeed);
        expect(pm.x.maxAccel).toBe(dm.x.maxAccel);
        expect(pm.y.stepsPerUnit).toBe(dm.y.stepsPerUnit);
        expect(pm.heads[0]!.z.stepsPerUnit).toBe(dm.heads[0]!.z.stepsPerUnit);
        expect(pm.heads[0]!.a.stepsPerUnit).toBe(dm.heads[0]!.a.stepsPerUnit);
    });

    it("quality defaults match code qualityConfig()", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        const q = r.config.quality;
        const def = qualityConfig();
        expect(q.chordTol).toBe(def.chordTol);
        expect(q.dvMax).toBe(def.dvMax);
        expect(q.junctionDeviation).toBe(def.junctionDeviation);
    });

    it("toolProfiles registry includes all 4 presets", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.toolProfiles.pen).toBe(PEN);
        expect(r.config.toolProfiles.knife).toBe(KNIFE);
        expect(r.config.toolProfiles.revolver_pen).toBe(REVOLVER_PEN);
    });
});

// ── optional fields default correctly ─────────────────────────────────────────

describe("configLoader: optional defaults", () => {
    it("rapid.feed defaults to 80 when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.rapid;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.rapid.feed).toBe(80);
    });

    it("z.feed defaults to 20 when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.z;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.z.feed).toBe(20);
    });

    it("slew defaults to empty (A ceiling) when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.slew;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.slew).toEqual({});
    });

    it("defaultHead defaults to 0 when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.defaultHead;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.defaultHead).toBe(0);
    });

    it("head xOffset/yOffset default to 0 when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.heads[0].xOffset;
        delete json.heads[0].yOffset;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.heads[0]!.xOffset).toBe(0);
        expect(r.config.machine.heads[0]!.yOffset).toBe(0);
    });

    it("axis maxRate/accel default to 0 when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.x.maxFeed;
        delete json.machine.x.maxAccel;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.x.maxFeed).toBe(0);
        expect(r.config.machine.x.maxAccel).toBe(0);
    });

    it("invert defaults to false when absent", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.x.invert;
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.x.invert).toBe(false);
    });

    it("laser is undefined when absent", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.laser).toBeUndefined();
    });

    it("peripherals default to empty array when absent", () => {
        const r = parseConfig(TEST_MACHINE);
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.peripherals).toEqual([]);
    });
});

// ── tool preset patching ──────────────────────────────────────────────────────

describe("configLoader: tool preset patching", () => {
    it("patches knife path.feed without changing other fields", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.tools = { knife: { path: { feed: 60 } } };
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        const knife = r.config.toolProfiles.knife!;
        expect(knife.path?.feed).toBe(60);
        expect(knife.tangential).toBe(KNIFE.tangential); // unchanged
        expect(knife.unwind).toBe(KNIFE.unwind); // unchanged
    });

    it("patches revolver_pen toolOffset", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.tools = {
            revolver_pen: { toolOffset: { xOffset: 0, yOffset: -15 } },
        };
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        const rp = r.config.toolProfiles.revolver_pen!;
        expect(rp.toolOffset).toEqual({ xOffset: 0, yOffset: -15 });
        expect(rp.slotOffsets).toEqual(REVOLVER_PEN.slotOffsets); // unchanged
    });

    it("errors on unknown tool preset name in tools", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.tools = { bogus: { path: { feed: 50 } } };
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) {
            expect(r.errors.some((e) => e.includes("tools.bogus"))).toBe(true);
        }
    });
});

// ── quality overrides ─────────────────────────────────────────────────────────

describe("configLoader: quality overrides", () => {
    it("patches only the specified quality fields", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.quality = { chordTol: 0.005, junctionDeviation: 0.1 };
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        const q = r.config.quality;
        expect(q.chordTol).toBe(0.005);
        expect(q.junctionDeviation).toBe(0.1);
        expect(q.dvMax).toBe(qualityConfig().dvMax); // unchanged
        expect(q.dsMax).toBe(qualityConfig().dsMax); // unchanged
    });
});

// ── dual-head + laser ─────────────────────────────────────────────────────────

describe("configLoader: dual-head + laser", () => {
    const dualHeadJson = `{
        "machine": {
            "fCpu": 150000000,
            "x": { "node": { "id": 1 }, "stepsPerUnit": 160 },
            "y": { "node": { "id": 2 }, "stepsPerUnit": 160 },
            "laser": { "xOffset": 0, "yOffset": 0 }
        },
        "heads": [
            {
                "tool": "knife",
                "xOffset": -50,
                "yOffset": 0,
                "z": { "node": { "id": 3 }, "stepsPerUnit": 1200 },
                "a": { "node": { "id": 4 }, "stepsPerUnit": 51.667, "rotary": true }
            },
            {
                "tool": "pen",
                "xOffset": 50,
                "yOffset": 0,
                "z": { "node": { "id": 5 }, "stepsPerUnit": 1200 },
                "a": { "node": { "id": 6 }, "stepsPerUnit": 51.667, "rotary": true }
            }
        ],
        "defaultHead": 0
    }`;

    it("parses two heads with different tools and offsets", () => {
        const r = parseConfig(dualHeadJson);
        expect(r.ok).toBe(true);
        if (!r.ok) return;
        expect(r.config.machine.heads).toHaveLength(2);
        expect(r.config.machine.heads[0]!.profile).toBe(KNIFE);
        expect(r.config.machine.heads[0]!.xOffset).toBe(-50);
        expect(r.config.machine.heads[1]!.profile).toBe(PEN);
        expect(r.config.machine.heads[1]!.xOffset).toBe(50);
    });

    it("parses laser pointer", () => {
        const r = parseConfig(dualHeadJson);
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.laser).toEqual({ xOffset: 0, yOffset: 0 });
    });
});

// ── peripherals ───────────────────────────────────────────────────────────────

describe("configLoader: peripherals", () => {
    it("parses peripherals with type and present defaults", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.peripherals = [
            { "id": 5, "type": 0x03 },
            { "id": 6, "type": 0x02, "present": false }
        ];
        const r = parseConfig(JSON.stringify(json));
        if (!r.ok) throw new Error("expected ok");
        expect(r.config.machine.peripherals).toHaveLength(2);
        expect(r.config.machine.peripherals[0]!.id).toBe(5);
        expect(r.config.machine.peripherals[0]!.type).toBe(0x03);
        expect(r.config.machine.peripherals[0]!.present).toBe(true); // default
        expect(r.config.machine.peripherals[1]!.present).toBe(false);
    });
});

// ── error cases ───────────────────────────────────────────────────────────────

describe("configLoader: error cases", () => {
    it("rejects invalid JSON", () => {
        const r = parseConfig("{ not valid json");
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors[0]).toContain("JSON parse error");
    });

    it("rejects non-object root", () => {
        const r = parseConfig("[1, 2, 3]");
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors[0]).toContain("root must be an object");
    });

    it("rejects missing machine", () => {
        const r = parseConfig('{"heads": []}');
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("machine"))).toBe(true);
    });

    it("rejects missing heads array", () => {
        const r = parseConfig('{"machine": {"fCpu": 150000000, "x": {"node": {"id": 1}, "stepsPerUnit": 160}, "y": {"node": {"id": 2}, "stepsPerUnit": 160}}}');
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("heads"))).toBe(true);
    });

    it("rejects empty heads array", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.heads = [];
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("non-empty"))).toBe(true);
    });

    it("rejects missing fCpu", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.fCpu;
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("fCpu"))).toBe(true);
    });

    it("rejects zero fCpu", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.machine.fCpu = 0;
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("fCpu"))).toBe(true);
    });

    it("rejects missing stepsPerUnit", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.x.stepsPerUnit;
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("stepsPerUnit"))).toBe(true);
    });

    it("rejects zero stepsPerUnit", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.machine.x.stepsPerUnit = 0;
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("stepsPerUnit"))).toBe(true);
    });

    it("rejects missing node.id", () => {
        const json = JSON.parse(TEST_MACHINE);
        delete json.machine.x.node.id;
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("id"))).toBe(true);
    });

    it("rejects unknown tool name in head", () => {
        const json = JSON.parse(TEST_MACHINE);
        json.heads[0].tool = "bogus";
        const r = parseConfig(JSON.stringify(json));
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("bogus"))).toBe(true);
    });

    it("collects MULTIPLE errors (not just the first)", () => {
        const r = parseConfig('{"machine": {}}');
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.length).toBeGreaterThan(1);
    });
});
