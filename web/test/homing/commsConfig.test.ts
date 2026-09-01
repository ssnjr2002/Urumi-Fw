/**
 * commsConfig.test.ts — demo/comms.json is the machine's live config, so load it.
 *
 * Nothing else in the suite reads it: every other test builds a machine from
 * test/machines.ts, which by design cannot catch a typo in the real file. The
 * homing block in particular is new, all-or-nothing, and only exercised when an
 * operator presses Home — a missing field would surface at the machine rather
 * than here.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { loadConfig } from "../../src/machine/json/load.js";
import { derivePlan } from "../../src/homing/derive.js";

const raw = readFileSync(new URL("../../demo/comms.json", import.meta.url), "utf8");

describe("demo/comms.json", () => {
    const result = loadConfig(raw);

    it("loads with no errors", () => {
        expect(result.ok ? [] : result.errors).toEqual([]);
        expect(result.ok).toBe(true);
    });

    it("gives X and Y a homing recipe, and derives a plan for each", () => {
        if (!result.ok) throw new Error("config did not load");
        for (const [letter, axis] of [
            ["x", result.config.machine.x],
            ["y", result.config.machine.y],
        ] as const) {
            expect(axis.homing).toBeDefined();
            const plan = derivePlan(letter, axis);
            expect(plan.legs).toHaveLength(4);
            // The datum is the axis's position after leg 4, not zero — the
            // far-end switch puts it near the top of travel.
            expect(plan.datumSteps).toBeGreaterThan(0);
        }
    });

    it("leaves Z and A without one — they have no limit switch fitted", () => {
        if (!result.ok) throw new Error("config did not load");
        for (const head of result.config.machine.heads) {
            expect(head.z.homing).toBeUndefined();
            expect(head.a.homing).toBeUndefined();
        }
    });
});
