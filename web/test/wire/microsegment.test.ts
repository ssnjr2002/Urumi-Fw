/**
 * Tests for wire/microsegment — MicroSegment type, flag constants, interval().
 * Fresh (no Python test_microsegment.py to port).
 */

import { describe, it, expect } from "vitest";
import {
    MICRO_PATH_END,
    MICRO_LIFT,
    MICRO_JOG,
    microSegment,
    interval,
} from "../../src/wire/microsegment.js";
import {
    resolvedAxes,
    qualityConfig,
} from "../../src/config/config.js";
import {
    defaultConfig,
} from "../../src/config/fixtures.js";

const axes = resolvedAxes(defaultConfig().machine);
const q = qualityConfig();

describe("wire: MicroSegment type", () => {
    it("microSegment() builds the 6-field record", () => {
        const m = microSegment(10, -5, 0, 3, 1500, MICRO_JOG);
        expect(m.dx).toBe(10);
        expect(m.dy).toBe(-5);
        expect(m.dz).toBe(0);
        expect(m.da).toBe(3);
        expect(m.interval).toBe(1500);
        expect(m.flags).toBe(MICRO_JOG);
    });

    it("flags default to 0", () => {
        const m = microSegment(1, 2, 3, 4, 100);
        expect(m.flags).toBe(0);
    });
});

describe("wire: flag constants", () => {
    it("MICRO_PATH_END = 0x01", () => {
        expect(MICRO_PATH_END).toBe(0x01);
    });
    it("MICRO_LIFT = 0x08", () => {
        expect(MICRO_LIFT).toBe(0x08);
    });
    it("MICRO_JOG = 0x10", () => {
        expect(MICRO_JOG).toBe(0x10);
    });
});

describe("wire: interval()", () => {
    it("pure-X move — no hypotenuse correction (hypot == major)", () => {
        // dx=160 steps = 1mm, v=80mm/s → stepRate = 80*160 = 12800
        // segTime = max(1/80, 160/(80*160)) = 0.0125s
        // cycles = 0.0125/160 * 150e6 = 11718
        const iv = interval(80, axes, q.vMin, 160, 0, 0, 0);
        expect(iv).toBe(11718);
    });

    it("diagonal — hypotenuse correction makes interval longer than pure-X", () => {
        // dx=160, dy=160 → distMm = sqrt(2) ≈ 1.414mm, major=160
        // segTime = sqrt(2)/80 ≈ 0.01768s (feed dominates the 0.0125s rate floor)
        // cycles = 0.01768/160 * 150e6 ≈ 16572
        const pureX = interval(80, axes, q.vMin, 160, 0, 0, 0);
        const diag = interval(80, axes, q.vMin, 160, 160, 0, 0);
        expect(diag).toBeGreaterThan(pureX);
        expect(diag).toBe(16572);
    });

    it("pure-A rotation — no XY feed, rate floor from A axis", () => {
        // dx=0, dy=0, da=100 → distMm=0, tRate = 100/(100*51.667) ≈ 0.01935s
        // cycles = 0.01935/100 * 150e6 ≈ 29031
        const iv = interval(80, axes, q.vMin, 0, 0, 0, 100);
        expect(iv).toBeGreaterThan(0);
        expect(iv).toBeLessThan(axes.fCpu);
        // A is slower than X — interval should be longer than a pure-X move
        const pureX = interval(80, axes, q.vMin, 160, 0, 0, 0);
        expect(iv).toBeGreaterThan(pureX);
    });

    it("per-axis rate limit — A axis rate floor bites on tight rotation", () => {
        // With a large da and no XY, the A rate floor governs.
        // A higher maxFeed should produce a shorter interval (faster allowed).
        const fastA = { ...axes, a: { ...axes.a, maxFeed: 1000 } };
        const slowA = { ...axes, a: { ...axes.a, maxFeed: 50 } };
        const ivFast = interval(80, fastA, q.vMin, 0, 0, 0, 200);
        const ivSlow = interval(80, slowA, q.vMin, 0, 0, 0, 200);
        expect(ivFast).toBeLessThan(ivSlow);
    });

    it("v=0 uses vMin floor", () => {
        // v=0 → vv = vMin = 0.5; distMm=1, segTime = 1/0.5 = 2.0s
        // cycles = 2.0/160 * 150e6 = 1875000
        const iv = interval(0, axes, q.vMin, 160, 0, 0, 0);
        expect(iv).toBe(1875000);
    });

    it("major=0 (all deltas zero) returns fCpu", () => {
        const iv = interval(80, axes, q.vMin, 0, 0, 0, 0);
        expect(iv).toBe(axes.fCpu);
    });

    it("legacy path (dx/dy undefined) — major-axis rate only", () => {
        // v=80, no dx/dy → stepRate = 80*160 = 12800, fCpu/stepRate = 11718
        const iv = interval(80, axes, q.vMin);
        expect(iv).toBe(11718);
    });

    it("interval never exceeds fCpu", () => {
        for (const v of [0, 0.5, 1, 10, 80, 1000]) {
            const iv = interval(v, axes, q.vMin, 160, 160, 100, 50);
            expect(iv).toBeLessThanOrEqual(axes.fCpu);
        }
    });

    it("interval is always >= 1", () => {
        for (const v of [0, 0.5, 1, 10, 80, 1000, 1e6]) {
            const iv = interval(v, axes, q.vMin, 1, 1, 1, 1);
            expect(iv).toBeGreaterThanOrEqual(1);
        }
    });
});
