/**
 * Tests for the duty-break post-pass (docs/tool_duty_limits.md).
 *
 * Fixtures are synthetic segment streams rather than baked toolpaths: the rule
 * under test is about TIMING and flags, and a real toolpath would make the
 * arithmetic unreadable without testing anything extra.
 *
 * fCpu is 1e6 throughout, so a segment of N major steps at interval I takes
 * exactly N·I/1e6 seconds — a 1 s cut is dx=1000, interval=1000.
 */

import { describe, it, expect } from "vitest";
import {
    scheduleDutyBreaks,
    segmentSeconds,
} from "../../src/production/dutyBreaks.js";
import {
    microSegment,
    MICRO_LIFT,
    MICRO_JOG,
    MICRO_PAUSE,
    MICRO_DUTY_RELEASE,
    MICRO_DUTY_ASSERT,
    type MicroSegment,
} from "../../src/wire/format/microsegment.js";
import { resolvedAxes, type ResolvedAxes } from "../../src/config/config.js";
import { uniformMachine } from "../../src/config/fixtures.js";
import type { DutyLimits } from "../../src/config/schema.js";

const F_CPU = 1e6;

/** ResolvedAxes for a machine whose Z may be inverted. */
function axesFor(zInvert: boolean): ResolvedAxes {
    const a = resolvedAxes(uniformMachine(160, 45.46, { fCpu: F_CPU }));
    return { ...a, z: { ...a.z, invert: zInvert } };
}

const duty = (o: Partial<DutyLimits> = {}): DutyLimits => ({
    maxOnS: 30, minOnS: 20, dwellS: 2, settleS: 0, ...o,
});

// ── fixture builders ─────────────────────────────────────────────────────────

/** A cut of `secs` seconds. */
const cut = (secs: number): MicroSegment =>
    microSegment(Math.round(secs * 1000), 0, 0, 0, 1000, 0);

/** A travel jog of `secs` seconds (fills an off-window). */
const jog = (secs: number): MicroSegment =>
    microSegment(Math.round(secs * 1000), 0, 0, 0, 1000, MICRO_JOG);

/** Z raise / lower, ~0.01 s each. Sign is in TRUE space; wire sign flips below. */
const zSeg = (trueDz: number, zInvert: boolean): MicroSegment =>
    microSegment(0, 0, zInvert ? -trueDz : trueDz, 0, 100, MICRO_LIFT);

const raise = (zInvert = false) => zSeg(+100, zInvert);
const lower = (zInvert = false) => zSeg(-100, zInvert);

/** cut(secs) … raise, gap, lower — one candidate with the given off-window. */
function stroke(cutS: number, gapS: number, zInvert = false): MicroSegment[] {
    return [cut(cutS), raise(zInvert), jog(gapS), lower(zInvert)];
}

const markedIndices = (segs: readonly MicroSegment[]): number[] =>
    segs.flatMap((s, i) => ((s.flags & MICRO_DUTY_RELEASE) !== 0 ? [i] : []));

// ── tests ────────────────────────────────────────────────────────────────────

describe("segmentSeconds", () => {
    it("times a segment by its MAJOR axis, not by interval alone", () => {
        // 1000 steps at 1000 cycles each = 1e6 cycles = 1 s at fCpu 1e6.
        expect(segmentSeconds(cut(1), F_CPU)).toBeCloseTo(1, 9);
        // A pure-A move is timed by da.
        expect(segmentSeconds(microSegment(0, 0, 0, 500, 1000, 0), F_CPU)).toBeCloseTo(0.5, 9);
    });
});

describe("scheduleDutyBreaks", () => {
    const axes = axesFor(false);

    it("marks nothing when the whole stream fits the budget", () => {
        const segs = [...stroke(5, 3), ...stroke(5, 3)];
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        expect(r.breaksAtS).toEqual([]);
        expect(markedIndices(r.segments)).toEqual([]);
    });

    it("marks a reset at a lift inside the band", () => {
        // Four 10 s strokes = ~40 s. One break needed, in (20, 30].
        const segs = [...stroke(10, 3), ...stroke(10, 3), ...stroke(10, 3), ...stroke(10, 3)];
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        expect(r.breaksAtS).toHaveLength(1);
        expect(r.breaksAtS[0]!).toBeGreaterThan(20);
        expect(r.breaksAtS[0]!).toBeLessThanOrEqual(30);
    });

    it("sets PAUSE + RELEASE + ASSERT together on the raise segment", () => {
        const segs = [...stroke(10, 3), ...stroke(10, 3), ...stroke(10, 3), ...stroke(10, 3)];
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        const idx = markedIndices(r.segments);
        expect(idx).toHaveLength(1);
        const s = r.segments[idx[0]!]!;
        // The marker must land on a lift, and must carry PAUSE — a duty marker
        // without PAUSE would never stop the machine, so the relay could not run.
        expect(s.flags & MICRO_LIFT).toBeTruthy();
        expect(s.flags & MICRO_PAUSE).toBeTruthy();
        expect(s.flags & MICRO_DUTY_RELEASE).toBeTruthy();
        expect(s.flags & MICRO_DUTY_ASSERT).toBeTruthy();
    });

    it("prefers a candidate whose off-window already covers the dwell", () => {
        // BOTH candidates must fall in the same band (20, 30] or this asserts
        // nothing: A at ~22 s with a 5 s gap (free, >= dwell 2 s), B at ~29 s
        // with a 0.2 s gap. Free wins even though B would cut for 7 s longer.
        const segs = [
            ...stroke(22, 5),    // candidate A — free
            ...stroke(2, 0.2),   // candidate B — later, would need an idle dwell
            cut(15),
        ];
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        expect(r.breaksAtS).toHaveLength(1);
        expect(r.breaksAtS[0]!).toBeCloseTo(22.01, 1); // A, not B
    });

    it("falls back to the latest candidate when none is free", () => {
        const segs = [
            ...stroke(21, 0.1),
            ...stroke(7, 0.1),   // latest in band — chosen
            ...stroke(20, 3),
        ];
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        expect(r.breaksAtS).toHaveLength(1);
        expect(r.breaksAtS[0]!).toBeGreaterThan(27);
    });

    it("schedules repeatedly over a long stream", () => {
        const segs = Array.from({ length: 12 }, () => stroke(10, 3)).flat();
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        // ~156 s of work on a 30 s budget — at least four resets.
        expect(r.breaksAtS.length).toBeGreaterThanOrEqual(4);
        // Never longer than maxOnS between consecutive resets.
        const stamps = [0, ...r.breaksAtS];
        for (let i = 1; i < stamps.length; i++) {
            expect(stamps[i]! - stamps[i - 1]!).toBeLessThanOrEqual(30);
        }
    });

    it("throws, naming the tool, when the band holds no lift", () => {
        // One unbroken 60 s cut: nothing to release at, and inserting a stop is
        // not implemented. Failing loudly beats overrunning the budget silently.
        const segs = [cut(60), raise(), jog(1), lower()];
        expect(() => scheduleDutyBreaks(segs, duty(), axes, F_CPU, "knife"))
            .toThrow(/'knife'.*no lift between/s);
    });

    it("reads raises in TRUE space, so an inverted Z is not mistaken for a lower", () => {
        const inv = axesFor(true);
        const segs = [
            ...stroke(10, 3, true), ...stroke(10, 3, true),
            ...stroke(10, 3, true), ...stroke(10, 3, true),
        ];
        const r = scheduleDutyBreaks(segs, duty(), inv, F_CPU);
        expect(r.breaksAtS).toHaveLength(1);
        // Marker must be on a RAISE. In wire space with invert, a raise has dz<0.
        const s = r.segments[markedIndices(r.segments)[0]!]!;
        expect(s.dz).toBeLessThan(0);
    });

    it("does not mutate the input", () => {
        const segs = [...stroke(10, 3), ...stroke(10, 3), ...stroke(10, 3), ...stroke(10, 3)];
        const before = segs.map((s) => s.flags);
        const r = scheduleDutyBreaks(segs, duty(), axes, F_CPU);
        expect(segs.map((s) => s.flags)).toEqual(before);
        expect(r.segments).not.toBe(segs);
    });
});
