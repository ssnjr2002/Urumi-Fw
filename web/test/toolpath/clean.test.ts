/**
 * Tests for clean.ts — stage 3 of the Bézier path.
 */

import { describe, it, expect } from "vitest";
import { cubic, lineToCubic, type CubicBezier, type Pt } from "../../src/toolpath/geometry.js";
import { cleanSubpath } from "../../src/toolpath/clean.js";

const pt = (x: number, y: number): Pt => ({ x, y });
const open = (...curves: CubicBezier[]) => ({ curves, closed: false });

describe("clean: zero-length curves", () => {
    it("drops them, joins their neighbours, keeps loops", () => {
        const tiny = cubic(pt(10, 0), pt(10, 0), pt(10, 0.00005), pt(10, 0.00005));
        const r = cleanSubpath(open(lineToCubic(pt(0, 0), pt(10, 0)), tiny, lineToCubic(pt(10, 0.00005), pt(10, 10))))!;
        expect(r.curves).toHaveLength(2);
        expect(r.curves[1]!.p0).toEqual(pt(10, 0));

        expect(cleanSubpath(open(cubic(pt(1, 1), pt(1, 1), pt(1, 1), pt(1, 1))))).toBeNull();

        const loop = cubic(pt(0, 0), pt(10, 10), pt(-10, 10), pt(0, 0));
        expect(cleanSubpath(open(loop))!.curves).toEqual([loop]);
    });
});

describe("clean: degenerate handles", () => {
    const cases = [
        { name: "p1 on p0", c: cubic(pt(0, 0), pt(0, 0), pt(6, 3), pt(9, 0)), p1: pt(2, 1), p2: pt(6, 3) },
        { name: "p1 and p2 on p0", c: cubic(pt(0, 0), pt(0, 0), pt(0, 0), pt(9, 0)), p1: pt(3, 0), p2: pt(0, 0) },
        { name: "p2 on p3", c: cubic(pt(0, 0), pt(3, 3), pt(9, 0), pt(9, 0)), p1: pt(3, 3), p2: pt(7, 1) },
        { name: "both", c: cubic(pt(0, 0), pt(0, 0), pt(9, 0), pt(9, 0)), p1: pt(3, 0), p2: pt(6, 0) },
    ];
    it.each(cases)("$name: moved a third toward the next distinct point", (k) => {
        const [c] = cleanSubpath(open(k.c))!.curves;
        expect(c!.p1.x).toBeCloseTo(k.p1.x, 12);
        expect(c!.p1.y).toBeCloseTo(k.p1.y, 12);
        expect(c!.p2.x).toBeCloseTo(k.p2.x, 12);
        expect(c!.p2.y).toBeCloseTo(k.p2.y, 12);
    });
});

describe("clean: closure", () => {
    const tri = [lineToCubic(pt(0, 0), pt(10, 0)), lineToCubic(pt(10, 0), pt(10, 10))];
    const cases = [
        { name: "closed, end near start: snapped", end: pt(0.005, 0.005), closed: true, n: 3, closedOut: true },
        { name: "open, end near start: snapped and closed", end: pt(0.005, 0.005), closed: false, n: 3, closedOut: true },
        { name: "open, end far from start: left open", end: pt(1, 1), closed: false, n: 3, closedOut: false },
        { name: "closed, end far from start: closing line added", end: pt(1, 1), closed: true, n: 4, closedOut: true },
    ];
    it.each(cases)("$name", (k) => {
        const last = cubic(pt(10, 10), pt(7, 7), pt(3, 3), k.end);
        const r = cleanSubpath({ curves: [...tri, last], closed: k.closed })!;
        expect(r.curves).toHaveLength(k.n);
        expect(r.closed).toBe(k.closedOut);
        if (k.closedOut) expect(r.curves[r.curves.length - 1]!.p3).toEqual(pt(0, 0));
        expect(r.joins).toHaveLength(k.closedOut ? k.n : k.n - 1);
    });

    it("a snap carries the last handle along", () => {
        const r = cleanSubpath({ curves: [...tri, cubic(pt(10, 10), pt(7, 7), pt(3, 3), pt(0.005, 0))], closed: true })!;
        expect(r.curves[2]!.p2.x).toBeCloseTo(2.995, 12);
    });
});

describe("clean: joins", () => {
    const cases: { name: string; b: CubicBezier; kind: string }[] = [
        { name: "C1", b: cubic(pt(30, 0), pt(40, 0), pt(50, 10), pt(60, 10)), kind: "smooth" },
        { name: "G1, unequal handles", b: cubic(pt(30, 0), pt(50, 0), pt(60, 0), pt(70, 0)), kind: "smooth" },
        { name: "4° turn", b: cubic(pt(30, 0), pt(40, 0.698), pt(50, 0.698), pt(60, 0)), kind: "smooth" },
        { name: "90° corner", b: cubic(pt(30, 0), pt(30, 10), pt(30, 20), pt(30, 30)), kind: "corner" },
        { name: "cusp", b: cubic(pt(30, 0), pt(20, 0), pt(10, 0), pt(0, 1)), kind: "corner" },
    ];
    it.each(cases)("$name: $kind", (k) => {
        const r = cleanSubpath(open(lineToCubic(pt(0, 0), pt(30, 0)), k.b))!;
        expect(r.joins).toEqual([k.kind]);
    });

    it("angleTol sets the threshold", () => {
        const sp = open(lineToCubic(pt(0, 0), pt(30, 0)), cubic(pt(30, 0), pt(40, 0.698), pt(50, 0.698), pt(60, 0)));
        expect(cleanSubpath(sp, { angleTol: 3 })!.joins).toEqual(["corner"]);
    });
});
