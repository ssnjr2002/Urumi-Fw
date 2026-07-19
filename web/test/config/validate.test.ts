/**
 * validate.test.ts — one describe() per rule in validate.ts.
 *
 * Rules are independent functions over a whole PipelineConfig, so each is
 * tested against a minimal hand-built config rather than a fixture machine.
 * Adding a rule should mean adding a describe() here and nothing else.
 *
 * The final describe covers loadConfig — the parse ⨟ validate composition,
 * where the two passes' contracts meet.
 */

import { describe, it, expect } from "vitest";
import {
    axisConfig,
    busNode,
    machineConfig,
    qualityConfig,
    toolHead,
    toolProfile,
    type MachineConfig,
    type PipelineConfig,
} from "../../src/config/schema.js";
import { KNIFE } from "../../src/config/tools.js";
import { validateConfig } from "../../src/config/validate.js";
import { loadConfig, parseConfig } from "../../src/config/load.js";

/** A sane baseline: capped X/Y, distinct node ids, one head. */
function cfg(machine?: Partial<Parameters<typeof machineConfig>[3]>): PipelineConfig {
    const m = machineConfig(
        axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }),
        axisConfig(busNode(2), 160, { maxFeed: 80, maxAccel: 1000 }),
        [
            toolHead(
                axisConfig(busNode(3), 300, { maxFeed: 10 }),
                axisConfig(busNode(4), 45, { maxFeed: 100, maxAccel: 500, rotary: true }),
            ),
        ],
        machine as Partial<MachineConfig>,
    );
    return { machine: m, quality: qualityConfig(), toolProfiles: {} };
}

const errors = (c: PipelineConfig) => validateConfig(c).errors;
const warnings = (c: PipelineConfig) => validateConfig(c).warnings;

describe("baseline", () => {
    it("a sane config produces no errors and no warnings", () => {
        expect(validateConfig(cfg())).toEqual({ errors: [], warnings: [] });
    });
});

describe("rule: nonNegativeCeilings", () => {
    it("errors on a negative axis ceiling", () => {
        const c = cfg();
        const bad: PipelineConfig = {
            ...c,
            machine: { ...c.machine, x: { ...c.machine.x, maxAccel: -1 } },
        };
        expect(errors(bad)).toContain("machine.x.maxAccel: must be >= 0");
    });

    it("0 is legal (uncapped), not an error", () => {
        const c = cfg();
        const zero: PipelineConfig = {
            ...c,
            machine: { ...c.machine, x: { ...c.machine.x, maxAccel: 0 } },
        };
        expect(errors(zero)).toEqual([]);
    });
});

describe("rule: nonNegativeTargets", () => {
    it("errors on a negative machine target feed", () => {
        expect(errors(cfg({ path: { feed: -5 } }))).toContain("machine.path.feed: must be >= 0");
    });

    it("errors on a negative tool target feed", () => {
        const c = cfg();
        const bad: PipelineConfig = {
            ...c,
            toolProfiles: { knife: toolProfile("knife", { path: { feed: -1 } }) },
        };
        expect(errors(bad)).toContain("tools.knife.path.feed: must be >= 0");
    });
});

describe("rule: xyMustBeCapped", () => {
    it("warns when X/Y feed is uncapped", () => {
        const c = cfg();
        const un: PipelineConfig = {
            ...c,
            machine: { ...c.machine, y: { ...c.machine.y, maxFeed: 0 } },
        };
        expect(warnings(un).some((w) => w.startsWith("machine.y.maxFeed"))).toBe(true);
    });

    it("does not warn about an uncapped Z or A (legitimately common)", () => {
        expect(warnings(cfg())).toEqual([]);
    });
});

describe("rule: targetsUnderCeilings", () => {
    it("warns when a tool path feed exceeds the XY ceiling", () => {
        const c = cfg();
        const over: PipelineConfig = {
            ...c,
            toolProfiles: { knife: toolProfile("knife", { path: { feed: 500 } }) },
        };
        expect(warnings(over)).toContain(
            "tools.knife.path.feed 500 exceeds axis ceiling 80 (clamped)",
        );
    });

    it("uses the INHERITED value when the tool does not override", () => {
        // machine.z.feed 20 > Z maxFeed 10 — the exact latent bug the value
        // model surfaced in the bench config.
        const c = cfg({ z: { feed: 20 } });
        const withTool: PipelineConfig = { ...c, toolProfiles: { knife: toolProfile("knife") } };
        expect(warnings(withTool)).toContain(
            "tools.knife.z.feed 20 exceeds axis ceiling 10 (clamped)",
        );
    });

    it("an uncapped (0) ceiling never triggers the warning", () => {
        const c = cfg();
        const uncapped: PipelineConfig = {
            ...c,
            machine: {
                ...c.machine,
                x: { ...c.machine.x, maxFeed: 0 },
                y: { ...c.machine.y, maxFeed: 0 },
            },
            toolProfiles: { knife: toolProfile("knife", { path: { feed: 9999 } }) },
        };
        expect(uncapped && warnings(uncapped).some((w) => w.includes("path.feed"))).toBe(false);
    });
});

describe("rule: uniqueNodeIds", () => {
    it("errors when two axes share a node id", () => {
        const dup = machineConfig(
            axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }),
            axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }), // ← same id
            [toolHead(axisConfig(busNode(3), 300), axisConfig(busNode(4), 45))],
        );
        const e = errors({ machine: dup, quality: qualityConfig(), toolProfiles: {} });
        expect(e.some((m) => m.includes("node id 1") && m.includes("machine.x, machine.y"))).toBe(
            true,
        );
    });

    it("errors when a peripheral collides with an axis", () => {
        const c = cfg({ peripherals: [busNode(3, { type: 0x02 })] });
        expect(errors(c).some((m) => m.includes("node id 3"))).toBe(true);
    });

    it("names every claimant so the operator knows what to edit", () => {
        const c = cfg({ peripherals: [busNode(4, { type: 0x02 })] });
        expect(errors(c)[0]).toContain("heads[0].a, peripherals[0]");
    });
});

describe("rule: defaultHeadInRange", () => {
    it("errors when defaultHead exceeds the head count", () => {
        expect(errors(cfg({ defaultHead: 2 }))).toContain(
            "defaultHead: 2 is out of range (machine has 1 head(s))",
        );
    });

    it("errors on a negative defaultHead", () => {
        expect(errors(cfg({ defaultHead: -1 })).length).toBe(1);
    });

    it("accepts the last valid index", () => {
        expect(errors(cfg({ defaultHead: 0 }))).toEqual([]);
    });
});

describe("rule: headsSupportSeededTools", () => {
    it("errors when a seeded tangential tool sits on a head with no A node", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }),
            axisConfig(busNode(2), 160, { maxFeed: 80, maxAccel: 1000 }),
            [
                toolHead(
                    axisConfig(busNode(3), 300),
                    axisConfig(busNode(4, { present: false }), 45),
                    { profile: KNIFE },
                ),
            ],
        );
        const e = errors({ machine: m, quality: qualityConfig(), toolProfiles: {} });
        expect(e.some((x) => x.includes("steers A"))).toBe(true);
    });

    it("says nothing about an empty socket", () => {
        expect(errors(cfg())).toEqual([]);
    });
});

// ── the two-pass composition ─────────────────────────────────────────────────

describe("loadConfig (parse ⨟ validate)", () => {
    const base = {
        machine: {
            x: { node: { id: 1 }, stepsPerUnit: 160, maxFeed: 80, maxAccel: 1000 },
            y: { node: { id: 2 }, stepsPerUnit: 160, maxFeed: 80, maxAccel: 1000 },
        },
        heads: [
            {
                tool: "knife",
                z: { node: { id: 3 }, stepsPerUnit: 300, maxFeed: 10 },
                a: { node: { id: 4 }, stepsPerUnit: 45, maxFeed: 100, maxAccel: 500 },
            },
        ],
        tools: { knife: { path: { feed: 40 }, z: { feed: 10 } } },
    };
    const json = (patch: object = {}) => JSON.stringify({ ...base, ...patch });

    it("accepts a config that is both well-formed and sane", () => {
        const r = loadConfig(json());
        expect(r.ok).toBe(true);
        // The knife overrides z.feed to 10; the OTHER presets inherit
        // machine.z.feed (20) and so exceed this machine's Z ceiling. That is a
        // true advisory about tools this machine could not run well, not noise.
        if (r.ok) {
            expect(r.warnings.every((w) => w.includes(".z.feed 20"))).toBe(true);
            expect(r.warnings.some((w) => w.includes("tools.knife"))).toBe(false);
        }
    });

    it("rejects a semantic error that parseConfig alone accepts", () => {
        // Duplicate node ids: structurally perfect, physically broken. This is
        // the gap that existed while validate was a pass nobody called.
        const dup = json({
            machine: {
                ...base.machine,
                y: { ...base.machine.y, node: { id: 1 } },
            },
        });
        expect(parseConfig(dup).ok).toBe(true);
        const r = loadConfig(dup);
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.some((e) => e.includes("node id 1"))).toBe(true);
    });

    it("surfaces warnings without blocking the load", () => {
        const r = loadConfig(
            json({ tools: { knife: { path: { feed: 500 } } } }),
        );
        expect(r.ok).toBe(true);
        if (r.ok) expect(r.warnings.some((w) => w.includes("exceeds axis ceiling"))).toBe(true);
    });

    it("propagates a structural error from the parse pass unchanged", () => {
        const r = loadConfig('{"machine": {}}');
        expect(r.ok).toBe(false);
        if (!r.ok) expect(r.errors.length).toBeGreaterThan(1);
    });
});
