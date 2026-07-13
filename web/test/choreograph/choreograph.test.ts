/**
 * Tests for choreograph — non-cutting motion emitters (stateless).
 * Fresh (no Python test_choreograph.py to port).
 */

import { describe, it, expect } from "vitest";
import {
    zMove,
    zStepCount,
    aMove,
    pivot,
    travelJog,
    preOrient,
    aMoveTo,
    headOffsetJog,
} from "../../src/choreograph/choreograph.js";
import { MICRO_JOG, MICRO_LIFT } from "../../src/wire/microsegment.js";
import {
    defaultConfig,
    resolvedAxes,
    KNIFE,
    PEN,
    CREASE,
    toolHead,
    axisConfig,
    busNode,
    machineConfig,
} from "../../src/config/config.js";

const axes = resolvedAxes(defaultConfig().machine);

describe("choreograph: zMove", () => {
    it("emits one MICRO_LIFT segment with invert applied", () => {
        // default Z: invert=true, stepsPerUnit=1200
        const m = zMove(100, axes, 20);
        expect(m.flags).toBe(MICRO_LIFT);
        expect(m.dz).toBe(-100); // invert flips sign
        expect(m.dx).toBe(0);
        expect(m.dy).toBe(0);
        expect(m.da).toBe(0);
    });

    it("negative dz becomes positive with invert", () => {
        const m = zMove(-100, axes, 20);
        expect(m.dz).toBe(100);
    });

    it("interval = fCpu / (zFeed * stepsPerUnit)", () => {
        // zRate = 20 * 1200 = 24000; interval = 150e6 / 24000 = 6250
        const m = zMove(100, axes, 20);
        expect(m.interval).toBe(6250);
    });
});

describe("choreograph: zStepCount", () => {
    it("computes rounded step count for lift height", () => {
        expect(zStepCount(2.0, axes)).toBe(2400); // 2.0 * 1200
    });

    it("returns 0 for zero or negative lift", () => {
        expect(zStepCount(0, axes)).toBe(0);
        expect(zStepCount(-1, axes)).toBe(0);
    });
});

describe("choreograph: aMove", () => {
    it("returns empty for da=0", () => {
        expect(aMove(0, axes)).toEqual([]);
    });

    it("emits MICRO_JOG segments with same da sign (invert applied)", () => {
        // default A: invert=true → positive da becomes negative emitted
        const segs = aMove(100, axes);
        expect(segs.length).toBeGreaterThan(0);
        for (const s of segs) {
            expect(s.flags).toBe(MICRO_JOG);
            expect(s.da).toBeLessThan(0); // invert flips
            expect(s.dx).toBe(0);
            expect(s.dy).toBe(0);
            expect(s.dz).toBe(0);
        }
    });

    it("telescopes: sum of |da| across segments == |da|", () => {
        const da = 1000;
        const segs = aMove(da, axes);
        const total = segs.reduce((sum, s) => sum + Math.abs(s.da), 0);
        expect(total).toBe(da);
    });

    it("trapezoidal — intervals vary (not all the same)", () => {
        // A large enough rotation to have accel + cruise + decel phases
        const segs = aMove(10000, axes);
        const intervals = segs.map((s) => s.interval);
        const unique = new Set(intervals);
        expect(unique.size).toBeGreaterThan(1);
    });

    it("intervals are within [1, fCpu]", () => {
        const segs = aMove(5000, axes);
        for (const s of segs) {
            expect(s.interval).toBeGreaterThanOrEqual(1);
            expect(s.interval).toBeLessThanOrEqual(axes.fCpu);
        }
    });
});

describe("choreograph: pivot", () => {
    it("with lift — emits raise + A rotation + lower", () => {
        const zSteps = 2400;
        const segs = pivot(100, true, zSteps, axes, 20);
        expect(segs.length).toBeGreaterThan(2); // zMove + aMove(several) + zMove
        // first and last are Z lifts
        expect(segs[0]!.flags & MICRO_LIFT).toBeTruthy();
        expect(segs[segs.length - 1]!.flags & MICRO_LIFT).toBeTruthy();
        // first Z is up (dz negative with invert), last Z is down (dz positive with invert)
        expect(segs[0]!.dz).toBeLessThan(0); // raise: invert flips +2400 to -2400
        expect(segs[segs.length - 1]!.dz).toBeGreaterThan(0); // lower: invert flips -2400 to +2400
    });

    it("without lift — emits only A rotation", () => {
        const segs = pivot(100, false, 0, axes, 20);
        expect(segs.length).toBeGreaterThan(0);
        // no Z lift segments
        for (const s of segs) {
            expect(s.flags & MICRO_LIFT).toBeFalsy();
            expect(s.dz).toBe(0);
        }
    });
});

describe("choreograph: travelJog", () => {
    it("emits one MICRO_JOG segment with invert applied", () => {
        // default X: invert=true, Y: invert=false
        const m = travelJog(0, 0, 160, 160, axes, 0.5, 80);
        expect(m).not.toBeNull();
        expect(m!.flags).toBe(MICRO_JOG);
        expect(m!.dx).toBe(-160); // X invert
        expect(m!.dy).toBe(160);  // Y no invert
    });

    it("returns null for zero movement", () => {
        const m = travelJog(100, 100, 100, 100, axes, 0.5, 80);
        expect(m).toBeNull();
    });
});

describe("choreograph: preOrient", () => {
    it("non-tangential profile (PEN) — returns empty, unchanged aPhys", () => {
        const result = preOrient(90, 0, 500, axes, PEN);
        expect(result.segments).toEqual([]);
        expect(result.newAPhys).toBe(500);
    });

    it("unwind (KNIFE) — rotates to absolute target", () => {
        // target = round(90 * 51.667) = 4650
        const result = preOrient(90, 0, 0, axes, KNIFE);
        expect(result.segments.length).toBeGreaterThan(0);
        expect(result.newAPhys).toBe(4650);
    });

    it("unwind (KNIFE) — compensates for accumulated rotation", () => {
        // Already at 4650 (90deg) → target for 90deg is 4650 → daTrue = 0
        const result = preOrient(90, 0, 4650, axes, KNIFE);
        expect(result.segments).toEqual([]);
        expect(result.newAPhys).toBe(4650);
    });

    it("non-unwind (CREASE) — rotates by delta from current theta", () => {
        // angleDelta(0, 90) = 90; daTrue = round(90 * 51.667) = 4650
        const result = preOrient(90, 0, 500, axes, CREASE);
        expect(result.segments.length).toBeGreaterThan(0);
        expect(result.newAPhys).toBe(500 + 4650);
    });

    it("non-unwind (CREASE) — zero delta returns empty", () => {
        const result = preOrient(45, 45, 500, axes, CREASE);
        expect(result.segments).toEqual([]);
        expect(result.newAPhys).toBe(500);
    });
});

describe("choreograph: aMoveTo (absolute A move)", () => {
    it("moves to 0° from a non-zero position (A-home)", () => {
        // A at 4650 steps (90deg) → aMoveTo(0, ...) rotates back by -4650
        const result = aMoveTo(0, 4650, axes);
        expect(result.segments.length).toBeGreaterThan(0);
        expect(result.newAPhys).toBe(0);
        // every segment is a JOG with only da
        for (const s of result.segments) {
            expect(s.flags).toBe(MICRO_JOG);
            expect(s.dx).toBe(0);
            expect(s.dy).toBe(0);
            expect(s.dz).toBe(0);
            expect(s.da).not.toBe(0);
        }
        // net da should be -4650 (invert applied: A invert=true → emitted -(-4650) = +4650)
        // but newAPhys is in TRUE (pre-invert) steps, so it's 0
        const netDa = result.segments.reduce((sum, s) => sum + s.da, 0);
        expect(netDa).toBe(4650); // invert flips the sign on emission
    });

    it("returns empty when already at the target", () => {
        const result = aMoveTo(90, 4650, axes); // 90deg = 4650 steps
        expect(result.segments).toEqual([]);
        expect(result.newAPhys).toBe(4650);
    });

    it("moves to a revolver slot offset (51.43deg)", () => {
        const slot1Deg = 360 / 7; // ≈ 51.4286
        const targetSteps = Math.round(slot1Deg * axes.a.stepsPerUnit);
        const result = aMoveTo(slot1Deg, 0, axes);
        expect(result.segments.length).toBeGreaterThan(0);
        expect(result.newAPhys).toBe(targetSteps);
    });

    it("handles negative targets (e.g. -90deg)", () => {
        const targetSteps = Math.round(-90 * axes.a.stepsPerUnit);
        const result = aMoveTo(-90, 0, axes);
        expect(result.newAPhys).toBe(targetSteps);
    });
});

describe("choreograph: headOffsetJog", () => {
    // Build a dual-head machine for testing: head 0 at (-50, 0), head 1 at (+50, 0)
    const z = axisConfig(busNode(3), 1200, { invert: true });
    const a = axisConfig(busNode(4), 51.667, { rotary: true, invert: true });
    const headL = toolHead(z, a, { xOffset: -50, yOffset: 0, profile: KNIFE });
    const headR = toolHead(z, a, { xOffset: 50, yOffset: 0, profile: PEN });
    const m = machineConfig(
        axisConfig(busNode(1), 160, { invert: true }),
        axisConfig(busNode(2), 160),
        [headL, headR],
    );
    const axes2 = resolvedAxes(m);

    it("returns null when offsets are identical", () => {
        const jog = headOffsetJog(headL, headL, axes2, 0.5, 80);
        expect(jog).toBeNull();
    });

    it("emits an XY jog with the delta between heads", () => {
        // from headL (-50) to headR (+50) → dxMm = 100, dyMm = 0
        const jog = headOffsetJog(headL, headR, axes2, 0.5, 80);
        expect(jog).not.toBeNull();
        // X invert=true → emitted -16000
        expect(jog!.dx).toBe(-16000);
        expect(jog!.dy).toBe(0);
        expect(jog!.flags).toBe(MICRO_JOG);
        expect(jog!.interval).toBeGreaterThan(0);
    });

    it("handles Y offset differences", () => {
        const headY = toolHead(z, a, { xOffset: 0, yOffset: 30, profile: PEN });
        const jog = headOffsetJog(headL, headY, axes2, 0.5, 80);
        expect(jog).not.toBeNull();
        // dxMm = 0 - (-50) = 50, dyMm = 30 - 0 = 30
        expect(jog!.dx).toBe(-8000); // X invert
        expect(jog!.dy).toBe(4800);  // Y no invert
    });

    it("returns null when both offsets match exactly", () => {
        const headSame = toolHead(z, a, { xOffset: -50, yOffset: 0, profile: PEN });
        const jog = headOffsetJog(headL, headSame, axes2, 0.5, 80);
        expect(jog).toBeNull();
    });
});
