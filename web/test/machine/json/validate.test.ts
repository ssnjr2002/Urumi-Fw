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
    NodeType,
    qualityConfig,
    toolHead,
    toolProfile,
    type MachineConfig,
    type PipelineConfig,
    ToolType,
} from "../../../src/machine/schema.js";
import { KNIFE } from "../../../src/machine/tools.js";
import { validateConfig } from "../../../src/machine/json/validate.js";
import { loadConfig, parseConfig } from "../../../src/machine/json/load.js";

/** A sane baseline: capped X/Y, distinct node ids, one head. */
function cfg(machine?: Partial<Parameters<typeof machineConfig>[3]>): PipelineConfig {
    const m = machineConfig(
        axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }),
        axisConfig(busNode(2), 160, { maxFeed: 80, maxAccel: 1000 }),
        [
            toolHead(
                axisConfig(busNode(3), 300, { maxFeed: 10 }),
                axisConfig(busNode(4), 45, { maxFeed: 100, maxAccel: 500, rotary: true }),
                { accepts: [ToolType.KNIFE] },
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

describe("rule: powerCoherent", () => {
    const knife = (power: object, machine?: Partial<MachineConfig>): PipelineConfig => {
        const c = cfg(machine);
        return { ...c, toolProfiles: { knife: toolProfile("knife", { ...KNIFE, id: 1, power } as any) } };
    };
    const periph = { peripherals: [busNode(8, { type: NodeType.KNIFE_OSC })] };

    it("accepts a present knife oscillator", () => {
        expect(errors(knife({ node: { id: 8, type: 3 }, settleS: 0.3 }, periph))).toEqual([]);
    });

    it("accepts the laser's node (a bench stand-in)", () => {
        expect(errors(knife({ node: { id: 3, type: 1 }, settleS: 0 }, { laser: { node: 3 } } as any))).toEqual([]);
    });

    it("errors on a node that is absent or not a knife oscillator", () => {
        const absent = { peripherals: [busNode(8, { type: NodeType.KNIFE_OSC, present: false })] };
        const msg = "tools.knife.power.node: {id: 8, type: 3} is not a present KNIFE_OSC " +
            "peripheral or the laser's STEPPER node";
        expect(errors(knife({ node: { id: 8, type: 3 }, settleS: 0 }, absent))).toContain(msg);
        expect(errors(knife({ node: { id: 8, type: 3 }, settleS: 0 }))).toContain(msg);
        expect(errors(knife({ node: { id: 3, type: 3 }, settleS: 0 }, { laser: { node: 3 } } as any)))
            .not.toEqual([]);
    });

    it("errors on a negative settle and an incoherent duty block", () => {
        const e = errors(knife({ node: { id: 8, type: 3 }, settleS: -1, dutyLimits: { maxOnS: 20, minOnS: 20, dwellS: 0 } }, periph));
        expect(e).toContain("tools.knife.power.settleS: must be >= 0 (got -1)");
        expect(e).toContain("tools.knife.power.dutyLimits.dwellS: must be > 0 (got 0)");
        expect(e).toContain("tools.knife.power.dutyLimits: minOnS (20) must be < maxOnS (20)");
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
    it("errors on a negative slew feed", () => {
        expect(errors(cfg({ slew: { feed: -5 } }))).toContain("machine.slew.feed: must be >= 0");
    });

    it("errors on a machine feed below 1", () => {
        expect(errors(cfg({ feed: { cutFallback: 0.5, travelDefault: 80, plungeFallback: 20, liftFeed: 20 } })))
            .toContain("machine.feed.cutFallback: must be >= 1 (got 0.5)");
    });

    it("errors on a negative tool target feed", () => {
        const c = cfg();
        const bad: PipelineConfig = {
            ...c,
            toolProfiles: { knife: toolProfile("knife", { cutFeed: -1 }) },
        };
        expect(errors(bad)).toContain("tools.knife.cutFeed: must be >= 0");
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
    it("warns when a tool cut feed exceeds the XY ceiling", () => {
        const c = cfg();
        const over: PipelineConfig = {
            ...c,
            toolProfiles: { knife: toolProfile("knife", { cutFeed: 500 }) },
        };
        expect(warnings(over)).toContain(
            "tools.knife.cutFeed 500 exceeds axis ceiling 80 (clamped)",
        );
    });

    it("uses the INHERITED value when the tool does not override", () => {
        // plungeFallback 20 > Z maxFeed 10 — the exact latent bug the value
        // model surfaced in the bench config.
        const c = cfg({ feed: { cutFallback: 80, travelDefault: 80, plungeFallback: 20, liftFeed: 20 } });
        const withTool: PipelineConfig = { ...c, toolProfiles: { knife: toolProfile("knife") } };
        expect(warnings(withTool)).toContain(
            "tools.knife.plungeFeed 20 exceeds axis ceiling 10 (clamped)",
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
            toolProfiles: { knife: toolProfile("knife", { cutFeed: 9999 }) },
        };
        expect(uncapped && warnings(uncapped).some((w) => w.includes("cutFeed"))).toBe(false);
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
        expect(errors(c)[0]).toContain("machine.heads[0].a, machine.peripherals[0]");
    });
});

describe("rule: defaultHeadInRange", () => {
    it("errors when defaultHead exceeds the head count", () => {
        expect(errors(cfg({ defaultHead: 2 }))).toContain(
            "machine.defaultHead: 2 is out of range (machine has 1 head(s))",
        );
    });

    it("errors on a negative defaultHead", () => {
        expect(errors(cfg({ defaultHead: -1 })).length).toBe(1);
    });

    it("accepts the last valid index", () => {
        expect(errors(cfg({ defaultHead: 0 }))).toEqual([]);
    });
});

describe("rule: headsHaveAxesForAccepted", () => {
    it("errors when an accepted tangential tool sits on a head with no A node", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }),
            axisConfig(busNode(2), 160, { maxFeed: 80, maxAccel: 1000 }),
            [
                toolHead(
                    axisConfig(busNode(3), 300),
                    axisConfig(busNode(4, { present: false }), 45),
                    { accepts: [ToolType.KNIFE] },
                ),
            ],
        );
        const e = errors({ machine: m, quality: qualityConfig(), toolProfiles: {} });
        expect(e.some((x) => x.includes("steers A"))).toBe(true);
    });

    it("says nothing when the head has the axes its accepted tools need", () => {
        expect(errors(cfg())).toEqual([]);
    });
});

// ── the two-pass composition ─────────────────────────────────────────────────

describe("loadConfig (parse ⨟ validate)", () => {
    const base = {
        machine: {
            meshOn: false,
            x: { node: { id: 1 }, stepsPerUnit: 160, maxFeed: 80, maxAccel: 1000 },
            y: { node: { id: 2 }, stepsPerUnit: 160, maxFeed: 80, maxAccel: 1000 },
            heads: [
                {
                    accepts: ["knife"],
                    z: { node: { id: 3 }, stepsPerUnit: 300, maxFeed: 10 },
                    a: { node: { id: 4 }, stepsPerUnit: 45, maxFeed: 100, maxAccel: 500 },
                },
            ],
        },
        tools: { knife: { id: 1, cutFeed: 40, plungeFeed: 10 } },
    };
    const json = (patch: object = {}) => JSON.stringify({ ...base, ...patch });

    it("accepts a config that is both well-formed and sane", () => {
        const r = loadConfig(json());
        expect(r.ok).toBe(true);
        // Only the listed knife exists, and it fits.
        if (r.ok) expect(r.warnings).toEqual([]);
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
            json({ tools: { knife: { id: 1, cutFeed: 500 } } }),
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
