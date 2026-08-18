/**
 * Tests for the Plan model — tool manifest + bake-time feasibility gate.
 */

import { describe, it, expect } from "vitest";
import { planToolTypes, planRequiredAxes, feasibleOn, type Plan } from "../../src/plan/plan.js";
import { AXIS_BITS } from "../../src/wire/format/status.js";
import { toolProfile } from "../../src/machine/index.js";
import {
    PEN,
    KNIFE,
    axisConfig,
    busNode,
    toolHead,
    machineConfig,
    ToolType,
    type ToolProfile,
    type MachineConfig,
} from "../../src/machine/index.js";

function plan(...profiles: ToolProfile[]): Plan {
    return { blocks: profiles.map((profile) => ({ profile, segments: [], startSteps: { x: 0, y: 0 } })) };
}

function machine(opts?: { aPresent?: boolean }): MachineConfig {
    const { aPresent = true } = opts ?? {};
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        [
            toolHead(
                axisConfig(busNode(3), 1200),
                axisConfig(busNode(4, { present: aPresent }), 51.667, { rotary: true }),
            ),
        ],
    );
}

describe("planToolTypes", () => {
    it("lists unique tool types in first-appearance order", () => {
        const p = plan(KNIFE, PEN, KNIFE, PEN);
        expect(planToolTypes(p)).toEqual([ToolType.KNIFE, ToolType.PEN]);
    });

    it("empty plan → empty list", () => {
        expect(planToolTypes({ blocks: [] })).toEqual([]);
    });
});

describe("feasibleOn (node presence)", () => {
    it("ok when every tool's nodes are wired", () => {
        const [ok, problems] = feasibleOn(plan(KNIFE, PEN), machine());
        expect(ok).toBe(true);
        expect(problems).toEqual([]);
    });

    it("flags the knife when the A node is absent", () => {
        const [ok, problems] = feasibleOn(plan(KNIFE, PEN), machine({ aPresent: false }));
        expect(ok).toBe(false);
        // pen has no A demand → only the knife is a problem
        expect(problems.map((p) => p[0])).toEqual(["knife"]);
        expect(problems[0]![1]).toContain("A axis node");
    });

    it("de-duplicates repeated infeasible tools", () => {
        const [, problems] = feasibleOn(plan(KNIFE, KNIFE), machine({ aPresent: false }));
        expect(problems).toHaveLength(1);
    });
});

describe("planRequiredAxes", () => {
    // Built explicitly rather than reached for from the presets: DEFAULTS.tool
    // sets liftHeight 0 and no preset overrides it, so every shipped profile is
    // currently a no-lift profile and a preset-based test would prove nothing
    // about the Z bit. (That is a property of the tool data, not of this
    // function — the orchestrate.js version it replaces behaved identically.)
    const flat = toolProfile("flat", { toolType: ToolType.PEN, liftHeight: 0, tangential: false });
    const lifts = toolProfile("lifts", { toolType: ToolType.PEN, liftHeight: 2, tangential: false });

    it("always requires X and Y, even for an empty plan", () => {
        expect(planRequiredAxes(plan())).toBe(AXIS_BITS.x | AXIS_BITS.y);
    });

    it("does not require Z for a tool that never lifts", () => {
        expect(planRequiredAxes(plan(flat)) & AXIS_BITS.z).toBe(0);
    });

    it("requires Z once any block lifts", () => {
        expect(planRequiredAxes(plan(flat, lifts)) & AXIS_BITS.z).toBe(AXIS_BITS.z);
    });

    it("requires A for a tangential tool", () => {
        expect(planRequiredAxes(plan(KNIFE)) & AXIS_BITS.a).toBe(AXIS_BITS.a);
        expect(planRequiredAxes(plan(PEN)) & AXIS_BITS.a).toBe(0);
    });

    it("is the union across blocks, not the last block's answer", () => {
        expect(planRequiredAxes(plan(KNIFE, lifts))).toBe(planRequiredAxes(plan(lifts, KNIFE)));
        // KNIFE contributes A (tangential), lifts contributes Z: all four.
        expect(planRequiredAxes(plan(KNIFE, lifts))).toBe(0x0f);
    });
});
