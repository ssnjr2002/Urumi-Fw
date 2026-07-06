/**
 * Tests for stage 1: SVG path -> cubic Beziers.
 * Ported from host/production/test_parse.py.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { KAPPA, type Pt } from "./bezier.js";
import { loadSvg, pathToCubics, circleToCubics } from "./parse.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "..", "..", "pipeline", "data");

function svg(name: string): string {
    return readFileSync(join(DATA, name), "utf-8");
}

function approxPt(a: Pt, b: Pt, tol = 1e-6): boolean {
    return Math.abs(a.x - b.x) < tol && Math.abs(a.y - b.y) < tol;
}

describe("stage 1: parse", () => {
    it("snake — S chains after C", () => {
        const curves = loadSvg(svg("test_snake.svg"));
        expect(curves).toHaveLength(2);
        expect(approxPt(curves[0]!.p0, { x: 10, y: 50 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 70, y: 50 })).toBe(true);
        expect(approxPt(curves[1]!.p0, { x: 70, y: 50 })).toBe(true);
        expect(approxPt(curves[1]!.p3, { x: 130, y: 50 })).toBe(true);
    });

    it("mixed commands — L degenerate, Q elevated, Z close", () => {
        const curves = loadSvg(svg("test.svg"));
        expect(curves).toHaveLength(3);
        // L: control points collinear with endpoints
        expect(approxPt(curves[0]!.p0, { x: 10, y: 10 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 90, y: 10 })).toBe(true);
        // Z: closes back to start
        expect(approxPt(curves[2]!.p3, { x: 10, y: 10 })).toBe(true);
    });

    it("saturate — implicit C repeat chains", () => {
        const curves = loadSvg(svg("test_saturate.svg"));
        expect(approxPt(curves[0]!.p3, { x: 50, y: 10 })).toBe(true);
        expect(approxPt(curves[1]!.p0, { x: 50, y: 10 })).toBe(true);
        expect(approxPt(curves[1]!.p3, { x: 90, y: 10 })).toBe(true);
    });

    it("saturate — S reflection after C", () => {
        const curves = loadSvg(svg("test_saturate.svg"));
        // curve[2] is C, curve[3] is S — p1 of [3] should be reflection of p2 of [2]
        const c = curves[2]!, s = curves[3]!;
        const reflected: Pt = { x: 2 * c.p3.x - c.p2.x, y: 2 * c.p3.y - c.p2.y };
        expect(approxPt(s.p1, reflected)).toBe(true);
    });

    it("saturate — S with no preceding C falls back to current point", () => {
        const curves = loadSvg(svg("test_saturate.svg"));
        expect(approxPt(curves[4]!.p0, curves[4]!.p1)).toBe(true);
    });

    it("saturate — Q degree elevation (2/3 rule)", () => {
        const curves = loadSvg(svg("test_saturate.svg"));
        const q = curves[6]!;
        const p0: Pt = { x: 10, y: 80 }, p3: Pt = { x: 90, y: 80 };
        const qp1: Pt = { x: 50, y: 60 };
        const c1: Pt = { x: p0.x + (2 / 3) * (qp1.x - p0.x), y: p0.y + (2 / 3) * (qp1.y - p0.y) };
        const c2: Pt = { x: p3.x + (2 / 3) * (qp1.x - p3.x), y: p3.y + (2 / 3) * (qp1.y - p3.y) };
        expect(approxPt(q.p1, c1)).toBe(true);
        expect(approxPt(q.p2, c2)).toBe(true);
    });

    it("saturate — relative m starts new subpath at correct position", () => {
        const curves = loadSvg(svg("test_saturate.svg"));
        // curve[27]: second subpath from "m 0 20" after L to (150,10)
        expect(approxPt(curves[27]!.p0, { x: 150, y: 30 })).toBe(true);
    });

    it("saturate — total curve count", () => {
        const curves = loadSvg(svg("test_saturate.svg"));
        expect(curves).toHaveLength(29);
    });

    it("circle — 4 cubics, kappa control arms, chain endpoints", () => {
        const curves = circleToCubics(50, 50, 40, 40);
        expect(curves).toHaveLength(4);
        for (let i = 0; i < 4; i++) {
            expect(approxPt(curves[i]!.p3, curves[(i + 1) % 4]!.p0)).toBe(true);
        }
        expect(approxPt(curves[0]!.p0, { x: 90, y: 50 })).toBe(true);
        const kx = 40 * KAPPA;
        expect(approxPt(curves[0]!.p1, { x: 90, y: 50 + kx })).toBe(true);
    });

    it("circle via loadSvg", () => {
        const curves = loadSvg(svg("test_circle.svg"));
        expect(curves).toHaveLength(4);
        expect(approxPt(curves[curves.length - 1]!.p3, curves[0]!.p0)).toBe(true);
    });

    it("ellipse element", () => {
        const curves = loadSvg(svg("test_ellipse.svg"));
        expect(curves).toHaveLength(4);
        // cx=50 cy=30 rx=40 ry=20 — rightmost point is (cx+rx, cy)
        expect(approxPt(curves[0]!.p0, { x: 90, y: 30 })).toBe(true);
        expect(approxPt(curves[curves.length - 1]!.p3, curves[0]!.p0)).toBe(true);
    });

    it("rect sharp corners", () => {
        const curves = loadSvg(svg("test_rect.svg"));
        expect(curves).toHaveLength(4);
        // x=10 y=20 w=80 h=60 -> corners (10,20)->(90,20)->(90,80)->(10,80)->back
        expect(approxPt(curves[0]!.p0, { x: 10, y: 20 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 90, y: 20 })).toBe(true);
        expect(approxPt(curves[3]!.p3, { x: 10, y: 20 })).toBe(true);
    });

    it("line element — degenerate cubic with 1/3 control points", () => {
        const curves = loadSvg(svg("test_line.svg"));
        expect(curves).toHaveLength(1);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 0 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 100, y: 50 })).toBe(true);
        expect(approxPt(curves[0]!.p1, { x: 100 / 3, y: 50 / 3 })).toBe(true);
    });

    it("polygon — closed triangle", () => {
        const curves = loadSvg(svg("test_polygon.svg"));
        expect(curves).toHaveLength(3);
        expect(approxPt(curves[0]!.p0, { x: 50, y: 10 })).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 90, y: 90 })).toBe(true);
        expect(approxPt(curves[2]!.p3, { x: 50, y: 10 })).toBe(true);
    });

    it("polyline — open, not closed", () => {
        const curves = loadSvg(
            '<svg xmlns="http://www.w3.org/2000/svg"><polyline points="0,0 50,50 100,0"/></svg>',
        );
        expect(curves).toHaveLength(2);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 0 })).toBe(true);
        expect(approxPt(curves[1]!.p3, { x: 100, y: 0 })).toBe(true);
    });

    it("pathToCubics — flat list from d string", () => {
        const curves = pathToCubics("M 0 0 L 10 0 L 10 10 Z");
        expect(curves).toHaveLength(3);
        expect(approxPt(curves[2]!.p3, { x: 0, y: 0 })).toBe(true);
    });
});
