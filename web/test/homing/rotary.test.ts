/**
 * rotary.test.ts — the two-sweep fold, pinned against real bench numbers.
 *
 * This is the arithmetic that turns two measurements into a datum, and it is
 * the one place in the rotary path where a mistake is SILENT: a wrong fold
 * produces a plausible index, a successful `setorigin`, and an A axis that is
 * some whole number of revolutions or one bias-width away from where it says it
 * is. Nothing downstream can detect that, so it is checked here against numbers
 * a machine actually produced.
 *
 * The bench run (2026-09-03, both heads clean in both directions, `idxcause ok
 * cross 3` throughout):
 *
 *   node 4  fwd: index 48947  steprev 16547     rev: index 15789  steprev 16562
 *   node 5  fwd: index  2137  steprev  1029     rev: index    74  steprev  1031
 */

import { describe, it, expect } from "vitest";
import { foldSigned, resolveRotaryIndex, deriveRotaryPlan } from "../../src/homing/derive.js";
import { axisConfig, busNode, type RotaryHoming } from "../../src/machine/schema.js";

const HEAD0 = { fi: 48947, fs: 16547, ri: 15789, rs: 16562 };
const HEAD1 = { fi: 2137, fs: 1029, ri: 74, rs: 1031 };

describe("foldSigned", () => {
    it("removes whole periods and keeps the remainder", () => {
        expect(foldSigned(33158, 16554.5)).toBeCloseTo(49, 6);
        expect(foldSigned(2063, 1030)).toBeCloseTo(3, 6);
    });

    it("returns a SIGNED result, not one just under a full period", () => {
        // The case the naive [0, period) modulo gets wrong. A bias of -3 steps
        // is a healthy machine measured in the other order; reporting it as
        // 1027 would fail a 2-degree tolerance check and send an operator
        // looking for a slipped belt that is not there.
        expect(foldSigned(-3, 1030)).toBeCloseTo(-3, 6);
        expect(foldSigned(-2063, 1030)).toBeCloseTo(-3, 6);
    });

    it("is symmetric: reversing the two sweeps negates the answer", () => {
        expect(foldSigned(-33158, 16554.5)).toBeCloseTo(-49, 6);
    });

    it("puts an exact half period at +period/2, not -period/2", () => {
        // Arbitrary but it must be DECIDED, or a magnet exactly opposite the
        // start point makes the datum flip a full half revolution between runs.
        expect(foldSigned(515, 1030)).toBeCloseTo(515, 6);
    });
});

describe("resolveRotaryIndex — bench data", () => {
    it("head 0 (node 4): a 49-step separation under two whole laps", () => {
        const r = resolveRotaryIndex(HEAD0.fi, HEAD0.fs, HEAD0.ri, HEAD0.rs);
        expect(r.stepsPerRev).toBeCloseTo(16554.5, 3);
        expect(r.revSpread).toBe(15);
        // Half of 49. The bias is the ONE-WAY error; the separation is twice it.
        expect(r.biasSteps).toBeCloseTo(24.5, 3);
        expect(r.indexSteps).toBeCloseTo(15813.5, 3);
    });

    it("head 1 (node 5): a 3-step separation on a 16x lighter gearing", () => {
        const r = resolveRotaryIndex(HEAD1.fi, HEAD1.fs, HEAD1.ri, HEAD1.rs);
        expect(r.stepsPerRev).toBeCloseTo(1030, 3);
        expect(r.revSpread).toBe(2);
        expect(r.biasSteps).toBeCloseTo(1.5, 3);
        expect(r.indexSteps).toBeCloseTo(75.5, 3);
    });

    it("the two heads agree on the bias IN DEGREES, not in steps", () => {
        // The finding that makes averaging the right fix rather than a stored
        // correction. In steps the two heads differ by 16x; in degrees they
        // agree to about 2%. A fixed TIME lag in the ADC would scale with
        // angular speed (5.4x apart here) and backlash would differ hugely
        // between a rigid and a belt-driven head, so neither survives this.
        const h0 = resolveRotaryIndex(HEAD0.fi, HEAD0.fs, HEAD0.ri, HEAD0.rs);
        const h1 = resolveRotaryIndex(HEAD1.fi, HEAD1.fs, HEAD1.ri, HEAD1.rs);
        const deg0 = (h0.biasSteps / h0.stepsPerRev) * 360;
        const deg1 = (h1.biasSteps / h1.stepsPerRev) * 360;
        expect(deg0).toBeCloseTo(0.53, 2);
        expect(deg1).toBeCloseTo(0.52, 2);
        expect(Math.abs(deg0 - deg1)).toBeLessThan(0.05);
    });

    it("swapping the sweeps moves the index by the same amount the other way", () => {
        const a = resolveRotaryIndex(HEAD0.fi, HEAD0.fs, HEAD0.ri, HEAD0.rs);
        const b = resolveRotaryIndex(HEAD0.ri, HEAD0.rs, HEAD0.fi, HEAD0.fs);
        expect(b.biasSteps).toBeCloseTo(-a.biasSteps, 6);
        // Both name the SAME physical point, one lap apart in the counter.
        expect(b.indexSteps - a.indexSteps).toBeCloseTo(2 * a.stepsPerRev, 3);
    });
});

describe("deriveRotaryPlan", () => {
    const homing: RotaryHoming = {
        kind: "rotary",
        budgetRevs: 4,
        pullInFeed: 18.12,
        sweepFeed: 54.37,
        rampSteps: 400,
        toleranceDeg: 2,
        datumDeg: 0,
    };
    const axis = axisConfig(busNode(4), 45.9847, { rotary: true, homing });

    it("sizes the budget off the NOMINAL revolution, not a measured one", () => {
        // No measurement exists on the first sweep of a new head, so the budget
        // has to come from the configured gearing. It is a 4x runaway ceiling,
        // which absorbs the sub-percent difference without noticing.
        const plan = deriveRotaryPlan("a", axis);
        expect(plan.nominalStepsPerRev).toBeCloseTo(16554.5, 0);
        expect(plan.legs[0]!.maxSteps).toBe(Math.round(45.9847 * 360 * 4));
    });

    it("converts the tolerance from degrees into steps", () => {
        const plan = deriveRotaryPlan("a", axis);
        expect(plan.toleranceSteps).toBe(Math.round(2 * 45.9847));
        // The measured 0.53-degree bias sits comfortably inside it, which is
        // the point: the check must pass on a healthy machine.
        expect(24.5).toBeLessThan(plan.toleranceSteps);
    });

    it("refuses an axis whose homing block is the wrong kind", () => {
        const linear = axisConfig(busNode(4), 45.9847, { rotary: true });
        expect(() => deriveRotaryPlan("a", linear)).toThrow(/no rotary homing config/);
    });
});
