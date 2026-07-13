/**
 * Tests for the Plan model — tool manifest + bake-time feasibility gate.
 */

import { describe, it, expect } from "vitest";
import { planToolTypes, feasibleOn, type Plan } from "../../src/plan/plan.js";
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
} from "../../src/config/config.js";

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
