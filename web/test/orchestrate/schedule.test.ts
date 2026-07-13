/**
 * Tests for the mount scheduler — the fill/execute/pause/swap batching.
 */

import { describe, it, expect } from "vitest";
import { scheduleMounts, mountDiff } from "../../src/orchestrate/schedule.js";
import type { Plan } from "../../src/plan/plan.js";
import {
    PEN,
    KNIFE,
    CREASE,
    REVOLVER_PEN,
    ToolType,
    type ToolProfile,
} from "../../src/config/config.js";

/** Build a plan from a tool sequence; slot tags a revolver block. */
function plan(...tools: (ToolProfile | [ToolProfile, number])[]): Plan {
    return {
        blocks: tools.map((t) => {
            const [profile, slot] = Array.isArray(t) ? t : [t, undefined];
            return slot === undefined
                ? { profile, segments: [], startSteps: { x: 0, y: 0 } }
                : { profile, slot, segments: [], startSteps: { x: 0, y: 0 } };
        }),
    };
}

describe("mountDiff", () => {
    it("reports tools added and removed", () => {
        const { added, removed } = mountDiff([ToolType.PEN, ToolType.KNIFE], [ToolType.KNIFE, ToolType.CREASE]);
        expect(added).toEqual([ToolType.CREASE]);
        expect(removed).toEqual([ToolType.PEN]);
    });

    it("empty diff for identical sets", () => {
        const { added, removed } = mountDiff([ToolType.PEN], [ToolType.PEN]);
        expect(added).toEqual([]);
        expect(removed).toEqual([]);
    });
});

describe("scheduleMounts", () => {
    it("empty plan → no phases", () => {
        expect(scheduleMounts({ blocks: [] }, 2).phases).toEqual([]);
    });

    it("rejects headCount < 1", () => {
        expect(() => scheduleMounts(plan(PEN), 0)).toThrow(/headCount/);
    });

    it("single pass when distinct tools fit the heads", () => {
        const s = scheduleMounts(plan(KNIFE, PEN, KNIFE), 2);
        expect(s.phases).toHaveLength(1);
        expect(s.phases[0]!.mount).toEqual([ToolType.KNIFE, ToolType.PEN]);
        expect(s.phases[0]!.blockIndices).toEqual([0, 1, 2]);
        expect(s.phases[0]!.swapIn).toEqual([ToolType.KNIFE, ToolType.PEN]);
        expect(s.phases[0]!.swapOut).toEqual([]);
    });

    it("splits into swap phases when tools exceed heads", () => {
        // 3 distinct tools, 2 heads → the crease can't be mounted with knife+pen
        const s = scheduleMounts(plan(KNIFE, PEN, CREASE, KNIFE), 2);
        expect(s.phases).toHaveLength(2);
        expect(s.phases[0]!.blockIndices).toEqual([0, 1]);
        expect(s.phases[1]!.mount).toEqual([ToolType.CREASE, ToolType.KNIFE]);
        expect(s.phases[1]!.blockIndices).toEqual([2, 3]);
        // swap: drop pen, load crease (knife stays)
        expect(s.phases[1]!.swapIn).toEqual([ToolType.CREASE]);
        expect(s.phases[1]!.swapOut).toEqual([ToolType.PEN]);
    });

    it("single head → a phase per tool change", () => {
        const s = scheduleMounts(plan(KNIFE, PEN, KNIFE), 1);
        expect(s.phases.map((p) => p.mount)).toEqual([
            [ToolType.KNIFE],
            [ToolType.PEN],
            [ToolType.KNIFE],
        ]);
        expect(s.phases.map((p) => p.blockIndices)).toEqual([[0], [1], [2]]);
    });

    it("keeps every block, in order, across all phases", () => {
        const s = scheduleMounts(plan(KNIFE, PEN, CREASE, KNIFE, PEN), 2);
        const all = s.phases.flatMap((p) => p.blockIndices);
        expect(all).toEqual([0, 1, 2, 3, 4]);
    });

    it("revolver slots share one head — no swap between slots", () => {
        const s = scheduleMounts(
            plan([REVOLVER_PEN, 0], [REVOLVER_PEN, 3], [REVOLVER_PEN, 1]),
            1,
        );
        expect(s.phases).toHaveLength(1);
        expect(s.phases[0]!.mount).toEqual([ToolType.REVOLVER_PEN]);
        expect(s.phases[0]!.blockIndices).toEqual([0, 1, 2]);
    });

    it("seedMounted suppresses a redundant first swap", () => {
        const s = scheduleMounts(plan(KNIFE, PEN), 2, [ToolType.PEN, ToolType.KNIFE]);
        expect(s.phases[0]!.swapIn).toEqual([]);
        expect(s.phases[0]!.swapOut).toEqual([]);
    });

    it("seedMounted reports the tools that must be removed", () => {
        const s = scheduleMounts(plan(KNIFE), 1, [ToolType.PEN]);
        expect(s.phases[0]!.swapIn).toEqual([ToolType.KNIFE]);
        expect(s.phases[0]!.swapOut).toEqual([ToolType.PEN]);
    });
});
