/**
 * Cubic Bezier primitives: point type, curve type, and degree-elevation
 * helpers. Ported from host/production/parse.py (stage 1) — the shared
 * types that all downstream stages consume.
 *
 * The pipeline/stages/bezier.py geometry math (point evaluation, derivatives,
 * arc length, curvature) will be added here when stage 4 is ported.
 */

export interface Pt {
    readonly x: number;
    readonly y: number;
}

export interface CubicBezier {
    readonly p0: Pt;
    readonly p1: Pt;
    readonly p2: Pt;
    readonly p3: Pt;
}

/** Cubic Bezier approximation constant for a quarter-circle arc. */
export const KAPPA = 0.5522847498;

export function cubic(p0: Pt, p1: Pt, p2: Pt, p3: Pt): CubicBezier {
    return { p0, p1, p2, p3 };
}

/** Degenerate cubic from a line: control points on the line at 1/3 and 2/3. */
export function lineToCubic(p0: Pt, p1: Pt): CubicBezier {
    const dx = (p1.x - p0.x) / 3;
    const dy = (p1.y - p0.y) / 3;
    return {
        p0,
        p1: { x: p0.x + dx, y: p0.y + dy },
        p2: { x: p1.x - dx, y: p1.y - dy },
        p3: p1,
    };
}

/** Degree elevation: quadratic -> cubic. C1 = P0 + 2/3*(QP1-P0), C2 = P2 + 2/3*(QP1-P2). */
export function quadToCubic(p0: Pt, qp1: Pt, p2: Pt): CubicBezier {
    return {
        p0,
        p1: { x: p0.x + (2 / 3) * (qp1.x - p0.x), y: p0.y + (2 / 3) * (qp1.y - p0.y) },
        p2: { x: p2.x + (2 / 3) * (qp1.x - p2.x), y: p2.y + (2 / 3) * (qp1.y - p2.y) },
        p3: p2,
    };
}
