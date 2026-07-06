/**
 * Tests for geometry.ts — vector algebra, endpoint tangents, and
 * Bezier math (point evaluation, derivatives, arc length, curvature).
 * Fresh (no Python test_bezier.py to port).
 */

import { describe, it, expect } from "vitest";
import {
    KAPPA,
    cubic,
    lineToCubic,
    sub,
    add,
    scale,
    length,
    normalize,
    angleBetweenDeg,
    exitTangent,
    entryTangent,
    bezierPoint,
    bezierDeriv1,
    bezierDeriv2,
    arcLength,
    curvature,
    type Pt,
} from "../src/geometry.js";

function approxPt(a: Pt, b: Pt, tol = 1e-9): boolean {
    return Math.abs(a.x - b.x) < tol && Math.abs(a.y - b.y) < tol;
}

// ── construction ──────────────────────────────────────────────────────────────

describe("geometry: construction", () => {
    it("cubic() builds the 4-point record", () => {
        const c = cubic({ x: 0, y: 0 }, { x: 1, y: 2 }, { x: 3, y: 4 }, { x: 5, y: 6 });
        expect(approxPt(c.p0, { x: 0, y: 0 })).toBe(true);
        expect(approxPt(c.p3, { x: 5, y: 6 })).toBe(true);
    });

    it("lineToCubic() puts control points at 1/3 and 2/3", () => {
        const c = lineToCubic({ x: 0, y: 0 }, { x: 9, y: 0 });
        expect(approxPt(c.p1, { x: 3, y: 0 })).toBe(true);
        expect(approxPt(c.p2, { x: 6, y: 0 })).toBe(true);
    });

    it("KAPPA is the quarter-circle approximation constant", () => {
        expect(KAPPA).toBeCloseTo(0.5522847498, 10);
    });
});

// ── vector algebra ────────────────────────────────────────────────────────────

describe("geometry: vector algebra", () => {
    it("sub / add / scale", () => {
        expect(approxPt(sub({ x: 5, y: 7 }, { x: 2, y: 1 }), { x: 3, y: 6 })).toBe(true);
        expect(approxPt(add({ x: 5, y: 7 }, { x: 2, y: 1 }), { x: 7, y: 8 })).toBe(true);
        expect(approxPt(scale({ x: 3, y: 4 }, 2), { x: 6, y: 8 })).toBe(true);
    });

    it("length of a 3-4-5 vector is 5", () => {
        expect(length({ x: 3, y: 4 })).toBe(5);
    });

    it("normalize: unit vector of (3,4) is (0.6, 0.8)", () => {
        const u = normalize({ x: 3, y: 4 });
        expect(approxPt(u, { x: 0.6, y: 0.8 }, 1e-9)).toBe(true);
    });

    it("normalize: near-zero vector returns (0,0)", () => {
        expect(approxPt(normalize({ x: 1e-13, y: 0 }), { x: 0, y: 0 })).toBe(true);
        expect(approxPt(normalize({ x: 0, y: 0 }), { x: 0, y: 0 })).toBe(true);
    });

    it("angleBetweenDeg: parallel = 0", () => {
        expect(angleBetweenDeg({ x: 1, y: 0 }, { x: 2, y: 0 })).toBeCloseTo(0, 6);
    });

    it("angleBetweenDeg: anti-parallel = 180", () => {
        expect(angleBetweenDeg({ x: 1, y: 0 }, { x: -1, y: 0 })).toBeCloseTo(180, 6);
    });

    it("angleBetweenDeg: perpendicular = 90", () => {
        expect(angleBetweenDeg({ x: 1, y: 0 }, { x: 0, y: 1 })).toBeCloseTo(90, 6);
    });

    it("angleBetweenDeg: clamps dot to [-1, 1] for near-parallel floats", () => {
        // u = v = unit vector -> dot slightly above 1 from float error must not throw
        const u = normalize({ x: 3, y: 4 });
        expect(() => angleBetweenDeg(u, u)).not.toThrow();
        expect(angleBetweenDeg(u, u)).toBeCloseTo(0, 6);
    });
});

// ── endpoint tangents ─────────────────────────────────────────────────────────

describe("geometry: endpoint tangents", () => {
    it("exitTangent of a rightward curve = (1, 0)", () => {
        const c = cubic({ x: 0, y: 0 }, { x: 5, y: 0 }, { x: 8, y: 0 }, { x: 10, y: 0 });
        expect(approxPt(exitTangent(c), { x: 1, y: 0 })).toBe(true);
    });

    it("entryTangent of an upward curve = (0, 1)", () => {
        const c = cubic({ x: 0, y: 0 }, { x: 0, y: 5 }, { x: 0, y: 8 }, { x: 0, y: 10 });
        expect(approxPt(entryTangent(c), { x: 0, y: 1 })).toBe(true);
    });

    it("endpoint tangents are unit vectors", () => {
        const c = cubic({ x: 1, y: 2 }, { x: 4, y: 6 }, { x: 8, y: 5 }, { x: 12, y: 9 });
        expect(length(exitTangent(c))).toBeCloseTo(1, 9);
        expect(length(entryTangent(c))).toBeCloseTo(1, 9);
    });

    it("degenerate (p2==p3) exitTangent returns (0,0)", () => {
        const c = cubic({ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 5, y: 5 }, { x: 5, y: 5 });
        expect(approxPt(exitTangent(c), { x: 0, y: 0 })).toBe(true);
    });
});

// ── bezierPoint ───────────────────────────────────────────────────────────────

describe("geometry: bezierPoint", () => {
    const c = cubic({ x: 0, y: 0 }, { x: 1, y: 2 }, { x: 3, y: 4 }, { x: 5, y: 6 });

    it("B(0) === p0", () => {
        expect(approxPt(bezierPoint(c, 0), c.p0)).toBe(true);
    });

    it("B(1) === p3", () => {
        expect(approxPt(bezierPoint(c, 1), c.p3)).toBe(true);
    });

    it("B(0.5) is the curve midpoint", () => {
        // De Casteljau midpoint of (0,0)(1,2)(3,4)(5,6):
        // mid01=(0.5,1), mid12=(2,3), mid23=(4,5)
        // mid012=(1.25,2), mid123=(3,4)
        // B(0.5) = (2.125, 3)
        const m = bezierPoint(c, 0.5);
        expect(approxPt(m, { x: 2.125, y: 3 }, 1e-9)).toBe(true);
    });

    it("straight line cubic: B(0.5) is the linear midpoint", () => {
        const line = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        expect(approxPt(bezierPoint(line, 0.5), { x: 5, y: 0 })).toBe(true);
    });
});

// ── bezierDeriv1 / bezierDeriv2 ───────────────────────────────────────────────

describe("geometry: bezierDeriv1", () => {
    const c = cubic({ x: 0, y: 0 }, { x: 1, y: 2 }, { x: 3, y: 4 }, { x: 5, y: 6 });

    it("B'(0) = 3*(p1 - p0)", () => {
        const d = bezierDeriv1(c, 0);
        expect(approxPt(d, { x: 3, y: 6 }, 1e-9)).toBe(true);
    });

    it("B'(1) = 3*(p3 - p2)", () => {
        const d = bezierDeriv1(c, 1);
        expect(approxPt(d, { x: 6, y: 6 }, 1e-9)).toBe(true);
    });

    it("B'(t) of a straight line cubic is constant = 3*(p3-p0)", () => {
        // lineToCubic: p1 = p0 + (p3-p0)/3, p2 = p3 - (p3-p0)/3
        // B'(t) = 3*[mt^2*(p1-p0) + 2mt*t*(p2-p1) + t^2*(p3-p2)]
        // For degenerate line, this collapses to (p3 - p0) * 3 * 1 = 3*(p3-p0)... actually:
        // p1-p0 = (p3-p0)/3, p2-p1 = (p3-p0)/3, p3-p2 = (p3-p0)/3
        // B'(t) = 3 * (p3-p0)/3 * [mt^2 + 2mt*t + t^2] = (p3-p0) * (mt+t)^2 = (p3-p0)
        const line = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        const d = bezierDeriv1(line, 0.5);
        expect(approxPt(d, { x: 10, y: 0 }, 1e-9)).toBe(true);
    });
});

describe("geometry: bezierDeriv2", () => {
    it("B''(t) of a straight line cubic is zero", () => {
        const line = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        const d2 = bezierDeriv2(line, 0.5);
        expect(approxPt(d2, { x: 0, y: 0 }, 1e-9)).toBe(true);
    });

    it("B''(0) = 6*(p2 - 2*p1 + p0)", () => {
        const c = cubic({ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 3, y: 0 }, { x: 6, y: 0 });
        const d2 = bezierDeriv2(c, 0);
        // 6 * (3 - 2*1 + 0) = 6 * 1 = 6
        expect(approxPt(d2, { x: 6, y: 0 }, 1e-9)).toBe(true);
    });
});

// ── arcLength ─────────────────────────────────────────────────────────────────

describe("geometry: arcLength", () => {
    it("straight line cubic: length = endpoint distance", () => {
        const line = lineToCubic({ x: 0, y: 0 }, { x: 3, y: 4 });
        // 3-4-5 triangle
        expect(arcLength(line)).toBeCloseTo(5, 6);
    });

    it("horizontal line: length = dx", () => {
        const line = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        expect(arcLength(line)).toBeCloseTo(10, 6);
    });

    it("quarter-circle approximation: length ≈ π/2 * r", () => {
        // 4-cubic kappa approximation of a unit circle quarter-arc
        // Each quarter: (1,0) -> (1, kappa) -> (kappa, 1) -> (0, 1)
        const quarter = cubic(
            { x: 1, y: 0 },
            { x: 1, y: KAPPA },
            { x: KAPPA, y: 1 },
            { x: 0, y: 1 },
        );
        const L = arcLength(quarter);
        // GL5 quadrature is exact for the integral it computes, but the kappa
        // cubic is itself only an approximation of a true circular arc — the
        // ~0.014% length error vs π/2 is the kappa approximation's error, not
        // the quadrature's. Loose tolerance reflects that.
        expect(L).toBeCloseTo(Math.PI / 2, 2);
    });
});

// ── curvature ─────────────────────────────────────────────────────────────────

describe("geometry: curvature", () => {
    it("straight line cubic: curvature ≈ 0 everywhere", () => {
        const line = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        for (const t of [0, 0.25, 0.5, 0.75, 1]) {
            expect(curvature(line, t)).toBeCloseTo(0, 9);
        }
    });

    it("curvature is non-negative", () => {
        const c = cubic({ x: 0, y: 0 }, { x: 1, y: 2 }, { x: 3, y: 4 }, { x: 5, y: 6 });
        for (const t of [0, 0.25, 0.5, 0.75, 1]) {
            expect(curvature(c, t)).toBeGreaterThanOrEqual(0);
        }
    });

    it("curvature of a circular-arc cubic at t=0.5 matches 1/r", () => {
        // Unit-circle quarter arc (r=1), so κ should be ≈ 1 at the midpoint.
        // The kappa cubic is only an approximation of a true arc — the ~0.6%
        // curvature error vs 1/r is the approximation's error, not a math bug.
        const quarter = cubic(
            { x: 1, y: 0 },
            { x: 1, y: KAPPA },
            { x: KAPPA, y: 1 },
            { x: 0, y: 1 },
        );
        expect(curvature(quarter, 0.5)).toBeCloseTo(1, 1);
    });

    it("curvature returns 0 for near-zero speed (degenerate)", () => {
        // p0=p1=p2=p3 -> B'(t) = 0 -> speed < 1e-10 -> returns 0
        const degenerate = cubic({ x: 5, y: 5 }, { x: 5, y: 5 }, { x: 5, y: 5 }, { x: 5, y: 5 });
        expect(curvature(degenerate, 0.5)).toBe(0);
    });
});
