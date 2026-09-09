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
import { derivePlan, deriveRotaryPlan } from "../../src/homing/derive.js";

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

    it("gives both heads' Z a homing recipe — a top switch, origin at the top", () => {
        if (!result.ok) throw new Error("config did not load");
        for (const head of result.config.machine.heads) {
            expect(head.z.homing).toBeDefined();
            const plan = derivePlan("z", head.z);
            expect(plan.legs).toHaveLength(4);
            // atOrigin: true — the switch sits at Z's 0 end, so the datum is
            // just parkMm off it, not hardTravel - parkMm. Read parkMm from the
            // config rather than repeating it: a literal here goes stale the
            // next time the recipe is tuned, and asserts the old value against
            // the new config.
            const parkMm = head.z.homing!.parkMm;
            expect(plan.datumSteps).toBe(Math.round(parkMm * head.z.stepsPerUnit));
        }
    });

    it("gives both heads' A a rotary recipe, and derives two sweeps for each", () => {
        if (!result.ok) throw new Error("config did not load");
        for (const head of result.config.machine.heads) {
            expect(head.a.homing?.kind).toBe("rotary");
            const plan = deriveRotaryPlan("a", head.a);
            expect(plan.legs).toHaveLength(2);
            // Identical but for direction. That symmetry IS the method: the two
            // answers straddle the truth by equal amounts only if the legs are
            // otherwise the same, so a config that let them differ would quietly
            // break the averaging rather than fail.
            const [fwd, rev] = plan.legs as [typeof plan.legs[0], typeof plan.legs[0]];
            expect(fwd!.dir).toBe(1);
            expect(rev!.dir).toBe(0);
            expect({ ...fwd!, dir: 0, describe: "" }).toEqual({ ...rev!, dir: 0, describe: "" });
            // The datum is the index itself, not an offset from a park point.
            expect(plan.datumSteps).toBe(0);
        }
    });

    // The bench commands these numbers were reverse-engineered from
    // (2026-09-03, both heads homing clean in both directions):
    //
    //   node 4:  home a 1 1200 400 400 66000 0   -> steprev 16547 / 16562
    //   node 5:  home a 1 2000 1200 400 200000 0 -> steprev 1029 / 1031
    //
    // Feeds are stored in deg/s because that is what an operator can check
    // against maxFeed, so this asserts the round trip back to the intervals
    // that actually ran. A drift here means the stored feed and the stored
    // stepsPerUnit no longer describe the same motion.
    it("reproduces the bench step intervals", () => {
        if (!result.ok) throw new Error("config did not load");
        const [head0, head1] = result.config.machine.heads;

        const p0 = deriveRotaryPlan("a", head0!.a);
        expect(p0.legs[0]!.startUs).toBe(1200);
        expect(p0.legs[0]!.floorUs).toBe(400);
        expect(p0.legs[0]!.rampSteps).toBe(400);
        // 4 revolutions of a ~16550-step revolution, against the 66000 run.
        expect(p0.legs[0]!.maxSteps).toBeGreaterThan(60000);
        expect(p0.legs[0]!.maxSteps).toBeLessThan(70000);

        const p1 = deriveRotaryPlan("a", head1!.a);
        expect(p1.legs[0]!.startUs).toBe(2000);
        expect(p1.legs[0]!.floorUs).toBe(1200);
        // NOT the 200000 the bench used. That was a bring-up guess of ~194
        // revolutions; the budget is a runaway ceiling and 4 revolutions is the
        // whole point of an evidence-terminated sweep.
        expect(p1.legs[0]!.maxSteps).toBe(4120);
    });

    // Measured, not configured. The rotary axis self-calibrates: steprev counts
    // microsteps, pulley teeth and gear ratio in one number, so the sweep's own
    // answer is a better stepsPerUnit than any datasheet arithmetic. These
    // assert the two heads really are different mechanisms -- head 1 is geared
    // 16x lighter -- because a copy-paste that gave them the same figure is
    // exactly the mistake this file exists to catch.
    it("carries each head's own measured gearing", () => {
        if (!result.ok) throw new Error("config did not load");
        const [head0, head1] = result.config.machine.heads;
        expect(head0!.a.stepsPerUnit * 360).toBeCloseTo(16554, -2);
        expect(head1!.a.stepsPerUnit * 360).toBeCloseTo(1030, -1);
    });
});
