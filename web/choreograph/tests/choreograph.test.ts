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
} from "../src/choreograph.js";
import { MICRO_JOG, MICRO_LIFT } from "../../wire/src/microsegment.js";
import {
    defaultConfig,
    resolvedAxes,
    KNIFE,
    PEN,
    CREASE,
} from "../../config/config.js";

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
