/**
 * estimate.test.ts — walk duration.
 *
 * The one real risk here is the interval-only bug: summing `interval` without
 * multiplying by the step count, which looks plausible and under-reports
 * enormously. The first test pins it.
 */

import { describe, it, expect } from "vitest";
import { walkSeconds, majorSteps, motionSegments } from "../../src/orchestrate/estimate.js";
import { microSegment } from "../../src/wire/format/microsegment.js";
import type { WalkEvent } from "../../src/orchestrate/walk.js";

const FCPU = 150_000_000;

const motion = (...segments: ReturnType<typeof microSegment>[]): WalkEvent => ({
    kind: "motion",
    segments,
});

describe("walkSeconds", () => {
    it("charges interval PER STEP, not per segment", () => {
        // 100 steps of X at 1000 cycles each = 100_000 cycles, not 1000.
        const seg = microSegment(100, 0, 0, 0, 1000);
        expect(walkSeconds([motion(seg)], FCPU)).toBeCloseTo(100_000 / FCPU, 12);
    });

    it("times against the dominant axis on a diagonal", () => {
        // Bresenham: the major axis steps every interval, minor axes less often,
        // so the segment costs max(|d|) intervals — not the sum.
        const seg = microSegment(30, 40, 0, 0, 500);
        expect(walkSeconds([motion(seg)], FCPU)).toBeCloseTo((40 * 500) / FCPU, 12);
    });

    it("sums across segments and events", () => {
        const a = motion(microSegment(10, 0, 0, 0, 100));
        const b = motion(microSegment(0, 20, 0, 0, 100), microSegment(0, 0, 5, 0, 200));
        const cycles = 10 * 100 + 20 * 100 + 5 * 200;
        expect(walkSeconds([a, b], FCPU)).toBeCloseTo(cycles / FCPU, 12);
    });

    it("charges nothing for a pause — it lasts as long as the operator does", () => {
        const events: WalkEvent[] = [
            { kind: "pause", swapIn: [], swapOut: [], mount: [] },
            motion(microSegment(10, 0, 0, 0, 100)),
        ];
        expect(walkSeconds(events, FCPU)).toBeCloseTo(1000 / FCPU, 12);
    });

    it("is zero for an empty walk", () => {
        expect(walkSeconds([], FCPU)).toBe(0);
    });

    it("scales inversely with fCpu", () => {
        const e = [motion(microSegment(10, 0, 0, 0, 100))];
        expect(walkSeconds(e, FCPU / 2)).toBeCloseTo(walkSeconds(e, FCPU) * 2, 12);
    });
});

describe("majorSteps", () => {
    it("takes the largest magnitude across all four axes, sign-blind", () => {
        expect(majorSteps(microSegment(-70, 40, 0, 0, 100))).toBe(70);
        expect(majorSteps(microSegment(0, 0, 0, -12, 100))).toBe(12);
    });
});

describe("motionSegments", () => {
    it("flattens motion events in order and drops pauses", () => {
        const events: WalkEvent[] = [
            motion(microSegment(1, 0, 0, 0, 10)),
            { kind: "pause", swapIn: [], swapOut: [], mount: [] },
            motion(microSegment(2, 0, 0, 0, 10), microSegment(3, 0, 0, 0, 10)),
        ];
        expect(motionSegments(events).map((s) => s.dx)).toEqual([1, 2, 3]);
    });
});
