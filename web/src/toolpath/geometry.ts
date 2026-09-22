/**
 * geometry.ts — cubic Bezier curve primitives, 2D vector algebra, and
 * Bezier geometry math.
 *
 * Three concerns, all geometric (not SVG concepts):
 *   - Curve types + construction helpers (Pt, CubicBezier, cubic,
 *     lineToCubic, quadToCubic, KAPPA). The SVG ingestion layer
 *     (../svg/ingest.ts) produces CubicBezier curves that conform to
 *     this contract; the toolpath stages (3+) consume them.
 *   - 2D vector algebra on Pt (sub, add, scale, length, normalize,
 *     angleBetweenDeg). Shared by every downstream stage.
 *   - Bezier endpoint tangents + Bezier math (point evaluation,
 *     first/second derivatives, arc length via 5-point Gauss-Legendre
 *     quadrature, curvature). Ported from pipeline/stages/bezier.py.
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

// ── 2D vector algebra on Pt ───────────────────────────────────────────────────

export function sub(a: Pt, b: Pt): Pt {
    return { x: a.x - b.x, y: a.y - b.y };
}

export function add(a: Pt, b: Pt): Pt {
    return { x: a.x + b.x, y: a.y + b.y };
}

export function scale(v: Pt, s: number): Pt {
    return { x: v.x * s, y: v.y * s };
}

export function length(v: Pt): number {
    return Math.sqrt(v.x * v.x + v.y * v.y);
}

/** Unit vector; returns {x:0, y:0} for near-zero input. */
export function normalize(v: Pt): Pt {
    const l = length(v);
    if (l < 1e-12) return { x: 0, y: 0 };
    return { x: v.x / l, y: v.y / l };
}

/** Signed angle from u to v in degrees, range [0, 180]. */
export function angleBetweenDeg(u: Pt, v: Pt): number {
    const dot = u.x * v.x + u.y * v.y;
    const clamped = Math.max(-1, Math.min(1, dot));
    return (Math.acos(clamped) * 180) / Math.PI;
}

// ── Bezier endpoint tangents ──────────────────────────────────────────────────

/** Unit tangent leaving curve c (direction p2 -> p3). Normalized B'(1). */
export function exitTangent(c: CubicBezier): Pt {
    return normalize(sub(c.p3, c.p2));
}

/** Unit tangent entering curve c (direction p0 -> p1). Normalized B'(0). */
export function entryTangent(c: CubicBezier): Pt {
    return normalize(sub(c.p1, c.p0));
}

// ── angle arithmetic ──────────────────────────────────────────────────────────

/** Shortest signed rotation from angle a to angle b in degrees, range ±180. */
export function angleDelta(a: number, b: number): number {
    let d = b - a;
    while (d > 180) d -= 360;
    while (d < -180) d += 360;
    return d;
}

// ── Bezier point evaluation + derivatives ─────────────────────────────────────
// Ported from pipeline/stages/bezier.py.

/** B(t) — De Casteljau evaluation of the cubic at parameter t in [0,1]. */
export function bezierPoint(c: CubicBezier, t: number): Pt {
    const mt = 1 - t;
    return {
        x: mt * mt * mt * c.p0.x + 3 * mt * mt * t * c.p1.x + 3 * mt * t * t * c.p2.x + t * t * t * c.p3.x,
        y: mt * mt * mt * c.p0.y + 3 * mt * mt * t * c.p1.y + 3 * mt * t * t * c.p2.y + t * t * t * c.p3.y,
    };
}

/** B'(t) — first derivative. */
export function bezierDeriv1(c: CubicBezier, t: number): Pt {
    const mt = 1 - t;
    return {
        x: 3 * (mt * mt * (c.p1.x - c.p0.x) + 2 * mt * t * (c.p2.x - c.p1.x) + t * t * (c.p3.x - c.p2.x)),
        y: 3 * (mt * mt * (c.p1.y - c.p0.y) + 2 * mt * t * (c.p2.y - c.p1.y) + t * t * (c.p3.y - c.p2.y)),
    };
}

/** B''(t) — second derivative. */
export function bezierDeriv2(c: CubicBezier, t: number): Pt {
    const mt = 1 - t;
    return {
        x: 6 * (mt * (c.p2.x - 2 * c.p1.x + c.p0.x) + t * (c.p3.x - 2 * c.p2.x + c.p1.x)),
        y: 6 * (mt * (c.p2.y - 2 * c.p1.y + c.p0.y) + t * (c.p3.y - 2 * c.p2.y + c.p1.y)),
    };
}

// Arc length: there is deliberately no arcLength() here. A 5-point
// Gauss-Legendre quadrature of |B'(t)| used to live at this spot with no
// production caller (audit F6) — flatten measures length by summing the
// samples it actually emits, which is both what the timeline needs and
// accurate on the near-cusp curves where GL5 is worst. Removed rather than
// ported: dead numerics are the most expensive kind of code to carry into C++.

/** κ(t) = |B'×B''| / |B'|³  (2D cross product = scalar). Returns 0 for near-zero speed. */
export function curvature(c: CubicBezier, t: number): number {
    const d1 = bezierDeriv1(c, t);
    const d2 = bezierDeriv2(c, t);
    const cross = d1.x * d2.y - d1.y * d2.x;
    const speed = Math.sqrt(d1.x * d1.x + d1.y * d1.y);
    if (speed < 1e-10) return 0;
    return Math.abs(cross) / (speed * speed * speed);
}
