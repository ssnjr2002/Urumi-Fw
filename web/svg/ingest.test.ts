/**
 * Tests for SVG ingestion (stages 1 + 2).
 * Merged from toolpath/parse.test.ts + normalise.test.ts.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { KAPPA, type Pt } from "../toolpath/geometry.js";
import {
    loadSvg,
    loadSvgMm,
    pathToCubics,
    circleToCubics,
    parseViewport,
} from "./ingest.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "..", "..", "pipeline", "data");

function svg(name: string): string {
    return readFileSync(join(DATA, name), "utf-8");
}

function approxPt(a: Pt, b: Pt, tol = 1e-6): boolean {
    return Math.abs(a.x - b.x) < tol && Math.abs(a.y - b.y) < tol;
}

// ── stage 1: parse ────────────────────────────────────────────────────────────

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

// ── stage 2: normalise ────────────────────────────────────────────────────────

describe("stage 2: normalise", () => {
    it("mm units — scale 2.0 (viewBox 100x100, 200mm canvas)", () => {
        const { viewport: vp } = loadSvgMm(svg("coord_mm_units.svg"));
        expect(vp.widthMm).toBe(200.0);
        expect(vp.heightMm).toBe(200.0);
        expect(vp.widthMm / vp.vbW).toBe(2.0);
    });

    it("mm units — SVG (10,10) -> (20, 180)mm after 2x scale + Y-flip", () => {
        const { curves } = loadSvgMm(svg("coord_mm_units.svg"));
        expect(approxPt(curves[0]!.p0, { x: 20, y: 180 }, 1e-3)).toBe(true);
    });

    it("cm units — 10cm = 100mm, scale 1.0", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_cm_units.svg"));
        expect(Math.abs(vp.widthMm - 100.0)).toBeLessThan(0.01);
        expect(approxPt(curves[0]!.p0, { x: 10, y: 90 }, 1e-3)).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 90, y: 10 }, 1e-3)).toBe(true);
    });

    it("px units — 377px at 96dpi ≈ 99.748mm", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_px_units.svg"));
        expect(Math.abs(vp.widthMm - 99.748)).toBeLessThan(0.01);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 99.748 }, 0.01)).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 99.748, y: 0 }, 0.01)).toBe(true);
    });

    it("nonzero viewBox origin — (50,30) -> (0, 80)mm", () => {
        const { curves } = loadSvgMm(svg("coord_nonzero_origin.svg"));
        expect(approxPt(curves[0]!.p0, { x: 0, y: 80 }, 1e-3)).toBe(true);
        expect(approxPt(curves[0]!.p3, { x: 100, y: 0 }, 1e-3)).toBe(true);
    });

    it("nonsquare — viewBox 200x100 -> 100x50mm, scale 0.5", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_nonsquare.svg"));
        expect(Math.abs(vp.widthMm - 100.0)).toBeLessThan(0.01);
        expect(Math.abs(vp.heightMm - 50.0)).toBeLessThan(0.01);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 50 }, 1e-3)).toBe(true);
        expect(approxPt(curves[2]!.p0, { x: 100, y: 0 }, 1e-3)).toBe(true);
    });

    it("no size fallback — viewBox px == mm 1:1, canvas 100x60mm", () => {
        const { curves, viewport: vp } = loadSvgMm(svg("coord_no_size.svg"));
        expect(vp.widthMm).toBe(100.0);
        expect(vp.heightMm).toBe(60.0);
        expect(approxPt(curves[0]!.p0, { x: 0, y: 60 }, 1e-3)).toBe(true);
    });

    it("Y-flip top edge — SVG y=0 -> machine y = height_mm = 100", () => {
        const { curves } = loadSvgMm(svg("coord_yfliip_verify.svg"));
        const topEdge = curves[0]!;
        expect(approxPt(topEdge.p0, { x: 0, y: 100 }, 1e-3)).toBe(true);
        expect(approxPt(topEdge.p3, { x: 100, y: 100 }, 1e-3)).toBe(true);
    });

    it("Y-flip bottom edge — SVG y=100 -> machine y = 0", () => {
        const { curves } = loadSvgMm(svg("coord_yfliip_verify.svg"));
        const bottomEdge = curves[1]!;
        expect(approxPt(bottomEdge.p0, { x: 0, y: 0 }, 1e-3)).toBe(true);
        expect(approxPt(bottomEdge.p3, { x: 100, y: 0 }, 1e-3)).toBe(true);
    });

    it("Y-flip diagonal — (0,0) -> (0,100); (100,100) -> (100,0)", () => {
        const { curves } = loadSvgMm(svg("coord_yfliip_verify.svg"));
        const diag = curves[3]!;
        expect(approxPt(diag.p0, { x: 0, y: 100 }, 1e-3)).toBe(true);
        expect(approxPt(diag.p3, { x: 100, y: 0 }, 1e-3)).toBe(true);
    });

    it("parseViewport — returns named fields", () => {
        const vp = parseViewport(svg("coord_mm_units.svg"));
        expect(vp.vbMinX).toBe(0);
        expect(vp.vbMinY).toBe(0);
        expect(vp.vbW).toBe(100);
        expect(vp.vbH).toBe(100);
        expect(vp.widthMm).toBe(200);
        expect(vp.heightMm).toBe(200);
    });
});
