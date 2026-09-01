/**
 * derive.test.ts — the four-leg plan is arithmetic, so test it as arithmetic.
 *
 * This is the payoff for keeping derive.ts pure. Every number here could send
 * an axis into a hard stop at seek speed on real hardware; none of them needs
 * hardware to check. The reference case is the X recipe confirmed on the bench
 * (docs/homing.md §7.1) at 160 steps/mm, so a regression shows up as a
 * disagreement with numbers a machine actually ran.
 */

import { describe, it, expect } from "vitest";
import { derivePlan, approachDir } from "../../src/homing/derive.js";
import { LegKind } from "../../src/homing/types.js";
import { axisConfig, busNode, type LinearHoming } from "../../src/machine/schema.js";

/** The bench X recipe: 500 mm of travel, far-end switch, 160 steps/mm. */
const X_HOMING: LinearHoming = {
    kind: "linear",
    hardTravel: 500,
    atOrigin: false,
    pullInFeed: 2.5,
    seekFeed: 12.5,
    latchFeed: 0.78,
    rampSteps: 400,
    backoffMm: 2,
    parkMm: 5,
};

const xAxis = (overrides = {}) =>
    axisConfig(busNode(1), 160, { maxFeed: 80, homing: X_HOMING, ...overrides });

describe("approachDir", () => {
    // atOrigin and invert are independent, and only their XOR is meaningful.
    // Enumerated rather than spot-checked because getting one of the four wrong
    // drives the axis away from its switch for the whole runaway budget.
    it.each([
        // atOrigin, invert, expected
        [false, false, 1],
        [false, true, 0],
        [true, false, 0],
        [true, true, 1],
    ])("atOrigin=%s invert=%s -> dir %i", (atOrigin, invert, expected) => {
        const axis = axisConfig(busNode(1), 160, { invert: invert as boolean });
        expect(approachDir(axis, { ...X_HOMING, atOrigin: atOrigin as boolean })).toBe(expected);
    });

    it("matches the bench: X (invert=false, far switch) approaches on dir 1", () => {
        expect(approachDir(xAxis(), X_HOMING)).toBe(1);
    });

    it("matches the bench: Y (invert=true, far switch) approaches on dir 0", () => {
        const y = axisConfig(busNode(2), 160, { invert: true });
        expect(approachDir(y, X_HOMING)).toBe(0);
    });
});

describe("derivePlan — legs", () => {
    const plan = derivePlan("x", xAxis());

    it("is four legs in seek/backoff/latch/park order", () => {
        expect(plan.legs.map((l) => l.kind)).toEqual([
            LegKind.SEEK, LegKind.BACKOFF, LegKind.LATCH, LegKind.PARK,
        ]);
    });

    it("alternates direction — a retract must undo the leg before it", () => {
        expect(plan.legs.map((l) => l.dir)).toEqual([1, 0, 1, 0]);
    });

    it("expects the switch held after each seek and clear after each retract", () => {
        expect(plan.legs.map((l) => l.endsLatched)).toEqual([true, false, true, false]);
    });

    it("converts feeds to intervals: 1e6 / (feed x stepsPerUnit)", () => {
        const seek = plan.legs[0]!;
        expect(seek.startUs).toBe(2500); // 2.5 mm/s  x 160 =   400 steps/s
        expect(seek.floorUs).toBe(500);  // 12.5 mm/s x 160 =  2000 steps/s
        expect(seek.rampSteps).toBe(400);
        expect(plan.legs[2]!.floorUs).toBe(8013); // 0.78 mm/s x 160
    });

    it("budgets the seek at hardTravel + 10%, so a home from the far end fits", () => {
        expect(plan.legs[0]!.maxSteps).toBe(88000); // 500 x 160 x 1.1
    });

    it("gives the retracts exact distances — a retract travels its whole budget", () => {
        expect(plan.legs[1]!.maxSteps).toBe(320); // 2 mm x 160
        expect(plan.legs[3]!.maxSteps).toBe(800); // 5 mm x 160
    });

    it("budgets the re-approach from the back-off, not the frame", () => {
        // Leg 3 starts 2 mm out and the switch is the only thing it can hit. A
        // hardTravel budget here would let a failed leg 2 run the whole axis.
        expect(plan.legs[2]!.maxSteps).toBe(800); // 320 x 2.5
        expect(plan.legs[2]!.maxSteps).toBeLessThan(plan.legs[0]!.maxSteps);
    });

    it("ramps only the seek — the slow legs have nothing to ramp from", () => {
        expect(plan.legs.slice(1).every((l) => l.rampSteps === 0)).toBe(true);
        expect(plan.legs.slice(1).every((l) => l.startUs === l.floorUs)).toBe(true);
    });
});

describe("derivePlan — the datum", () => {
    it("far-end switch: parks parkMm BELOW hardTravel", () => {
        // (500 - 5) x 160. Not zero: leg 4 leaves the axis near the far end.
        expect(derivePlan("x", xAxis()).datumSteps).toBe(79200);
    });

    it("origin-end switch: parks parkMm ABOVE zero", () => {
        const axis = xAxis({ homing: { ...X_HOMING, atOrigin: true } });
        expect(derivePlan("x", axis).datumSteps).toBe(800); // 5 x 160
    });

    it("atOrigin is the whole difference — a flip moves the origin by hardTravel", () => {
        const far = derivePlan("x", xAxis()).datumSteps;
        const near = derivePlan("x", xAxis({ homing: { ...X_HOMING, atOrigin: true } })).datumSteps;
        expect(far - near).toBe((500 - 10) * 160);
    });
});

describe("derivePlan — refusals", () => {
    it("throws for an axis with no switch rather than returning an empty plan", () => {
        // A silent empty plan would let a `home all` skip an axis and report ok.
        const noSwitch = axisConfig(busNode(3), 1200);
        expect(() => derivePlan("z", noSwitch)).toThrow(/no homing config/);
    });
});
