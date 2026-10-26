/**
 * Tests for annotate.ts — split and measure Béziers for the Pico.
 */

import { describe, it, expect } from "vitest";
import { bezierPoint, cubic, KAPPA, lineToCubic, type CubicBezier, type Pt } from "../../src/toolpath/geometry.js";
import type { CleanSubpath, JoinKind } from "../../src/toolpath/clean.js";
import {
    analyzeBezier,
    annotate,
    BezierFlag,
    DEFAULT_ANNOTATE_OPTIONS,
    type AnnotatedBezier,
    type BezierAnalysis,
} from "../../src/toolpath/annotate.js";

const pt = (x: number, y: number): Pt => ({ x, y });
const sub = (curves: CubicBezier[], joins: JoinKind[] = [], closed = false): CleanSubpath => ({ curves, joins, closed });
const near = (a: Pt, b: Pt, tol = 1e-9) => Math.hypot(a.x - b.x, a.y - b.y) < tol;
const ok = (c: CubicBezier) => {
    const a = analyzeBezier(c);
    if ("error" in a) throw new Error(a.error);
    return a as BezierAnalysis;
};

const R = 50;
const QUARTER = cubic(pt(R, 0), pt(R, R * KAPPA), pt(R * KAPPA, R), pt(0, R));
const S_CURVE = cubic(pt(0, 0), pt(10, 10), pt(20, -10), pt(30, 0));
const CUSP = cubic(pt(0, 0), pt(10, 10), pt(0, 10), pt(10, 0));
const TIGHT_THEN_LOOSE = cubic(pt(0, 0), pt(10, 0), pt(10, 1), pt(10, 30));

describe("annotate: analyzeBezier", () => {
    it("quarter circle: the circle's length and curvature", () => {
        const a = ok(QUARTER);
        expect(a.length).toBeCloseTo((Math.PI * R) / 2, 1);
        expect(a.kappaMax).toBeCloseTo(1 / R, 3);
        expect(a.kappaStart).toBeGreaterThan(0);
        expect(a.dkappaMax).toBeLessThan(1e-4);
        expect(a.fitError).toBeLessThan(1e-3);
    });

    it("a line as a cubic: c2 = c3 = 0, no curvature", () => {
        const a = ok(lineToCubic(pt(0, 0), pt(100, 0)));
        expect(a.ts[0]).toBeCloseTo(1 / 100, 12);
        expect(a.ts[1]).toBeCloseTo(0, 12);
        expect(a.ts[2]).toBeCloseTo(0, 12);
        expect(a.kappaMax).toBe(0);
    });

    it.each([
        { error: "DegenerateHandle", c: cubic(pt(0, 0), pt(0, 0), pt(5, 1), pt(10, 0)) },
        { error: "Cusp", c: CUSP },
        { error: "NonMonotonic", c: cubic(pt(0, 0), pt(0.1, 0), pt(0.3, 0), pt(100, 0)) },
    ])("refuses $error", ({ error, c }) => {
        expect(analyzeBezier(c)).toEqual({ error });
    });
});

/** Pieces rejoin, each is one the Pico takes, within the options. */
function expectValid(input: CubicBezier[], out: AnnotatedBezier[]) {
    expect(near(out[0]!.curve.p0, input[0]!.p0)).toBe(true);
    expect(near(out[out.length - 1]!.curve.p3, input[input.length - 1]!.p3)).toBe(true);
    for (let i = 1; i < out.length; i++) expect(near(out[i]!.curve.p0, out[i - 1]!.curve.p3)).toBe(true);
    for (const p of out) {
        const a = ok(p.curve);
        expect(a.fitError).toBeLessThanOrEqual(DEFAULT_ANNOTATE_OPTIONS.fitTol);
        expect(p.length).toBe(a.length);
    }
}

describe("annotate: splits", () => {
    it("an S-curve splits at its inflection", () => {
        const out = annotate(sub([S_CURVE]));
        expectValid([S_CURVE], out);
        expect(out.some((p) => near(p.curve.p3, bezierPoint(S_CURVE, 0.5), 1e-6))).toBe(true);
        for (const p of out) expect(p.kappaStart * p.kappaEnd).toBeGreaterThanOrEqual(0);
        expect(out.every((p) => !(p.flags & BezierFlag.BREAK))).toBe(true);
    });

    it("a cusp splits with BREAK", () => {
        const out = annotate(sub([CUSP]));
        expectValid([CUSP], out);
        const at = out.findIndex((p) => p.flags & BezierFlag.BREAK);
        expect(at).toBeGreaterThan(0);
        expect(near(out[at]!.curve.p0, bezierPoint(CUSP, 0.5), 1e-6)).toBe(true);
    });

    it("a tight-then-loose curve splits by the curvature ratio", () => {
        const out = annotate(sub([TIGHT_THEN_LOOSE]));
        expectValid([TIGHT_THEN_LOOSE], out);
        expect(out.length).toBeGreaterThan(1);
        const { kappaRatio, kappaFloor } = DEFAULT_ANNOTATE_OPTIONS;
        for (const p of out) {
            const ks = [p.kappaStart, p.kappaEnd].map((k) => Math.max(Math.abs(k), kappaFloor));
            expect(p.kappaMax / Math.min(...ks)).toBeLessThanOrEqual(kappaRatio * 1.05);
        }
    });

    it("no split leaves a piece shorter than minLength", () => {
        const spike = cubic(pt(0, 0), pt(0.02, 0), pt(30, 10), pt(50, 0));
        // The shortest piece is kept outside fitTol rather than halved.
        const out = annotate(sub([spike]));
        out.forEach((p) => ok(p.curve));
        expect(out.length).toBeGreaterThan(1);
        expect(Math.min(...out.map((b) => b.length))).toBeGreaterThanOrEqual(DEFAULT_ANNOTATE_OPTIONS.minLength * 0.99);
    });

    it("a line never splits", () => {
        expect(annotate(sub([lineToCubic(pt(0, 0), pt(100, 0))]))).toHaveLength(1);
    });
});

describe("annotate: flags", () => {
    const { START, BREAK, END } = BezierFlag;
    const a = lineToCubic(pt(0, 0), pt(10, 0));
    const b = lineToCubic(pt(10, 0), pt(20, 0));
    const c = lineToCubic(pt(10, 0), pt(10, 10));
    const back = lineToCubic(pt(10, 10), pt(0, 0));
    it.each([
        { name: "one piece is START and END", sp: sub([a]), flags: [START | END] },
        { name: "smooth join", sp: sub([a, b], ["smooth"]), flags: [START, END] },
        { name: "corner join is BREAK", sp: sub([a, c], ["corner"]), flags: [START, BREAK | END] },
        { name: "closed: wrap join ignored", sp: sub([a, c, back], ["corner", "corner", "corner"], true), flags: [START, BREAK, BREAK | END] },
    ])("$name", ({ sp, flags }) => {
        expect(annotate(sp).map((p) => p.flags)).toEqual(flags);
    });

    it.each([
        { name: "smooth: one stretch to the end", sp: sub([a, b], ["smooth"]), dist: [20, 0] },
        { name: "corner: each stretch to the next BREAK", sp: sub([a, c], ["corner"]), dist: [10, 10] },
        { name: "rounded up", sp: sub([lineToCubic(pt(0, 0), pt(10.2, 0))]), dist: [11] },
        { name: "capped at 65535", sp: sub([lineToCubic(pt(0, 0), pt(70000, 0))]), dist: [65535] },
    ])("nextBreakDist, $name", ({ sp, dist }) => {
        expect(annotate(sp).map((p) => p.nextBreakDist)).toEqual(dist);
    });

    // Quarter circles about the origin, counter-clockwise from `deg`.
    const arc = (deg: number) => {
        const r = (d: number) => (d * Math.PI) / 180;
        const p = (d: number) => pt(R * Math.cos(r(d)), R * Math.sin(r(d)));
        const t = (d: number) => pt(-Math.sin(r(d)) * R * KAPPA, Math.cos(r(d)) * R * KAPPA);
        const add = (a: Pt, b: Pt, k: number) => pt(a.x + k * b.x, a.y + k * b.y);
        return cubic(p(deg), add(p(deg), t(deg), 1), add(p(deg + 90), t(deg + 90), -1), p(deg + 90));
    };
    const ranges = (sp: CleanSubpath) => {
        const out = annotate(sp);
        // Each piece of a stretch counts from its own p0, so only the first is checked here.
        return out.filter((p) => p.flags & (START | BREAK)).map((p) => [p.sweepMin, p.sweepMax]);
    };
    it.each([
        { name: "a line turns nowhere", sp: sub([a]), r: [[0, 0]] },
        { name: "a quarter turns left", sp: sub([arc(0)]), r: [[0, 90]] },
        { name: "a reversed quarter turns right", sp: sub([cubic(arc(0).p3, arc(0).p2, arc(0).p1, arc(0).p0)]), r: [[-90, 0]] },
        { name: "smooth joins sum", sp: sub([arc(0), arc(90), arc(180), arc(270)], ["smooth", "smooth", "smooth"]), r: [[0, 360]] },
        { name: "a corner starts again", sp: sub([arc(0), a], ["corner"]), r: [[0, 90], [0, 0]] },
    ])("sweep, $name", ({ sp, r }) => {
        expect(ranges(sp)).toEqual(r);
    });

    it("sweep: a later piece counts from its own p0 to the stretch's end", () => {
        const out = annotate(sub([arc(0), arc(90), arc(180)], ["smooth", "smooth"]));
        expect(out.map((p) => [p.sweepMin, p.sweepMax])).toEqual([[0, 270], [0, 180], [0, 90]]);
    });
});
