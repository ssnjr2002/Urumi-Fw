/**
 * annotate.ts — split a cleaned subpath into Béziers the Pico can queue
 * without analysing them, and measure each the way the Pico does
 * (`lib/planner/bezier.cpp`).
 *
 * Splits, per curve: cusps (flagged BREAK), inflections, curvature ratio,
 * t(s) fit. Every piece comes from an exact de Casteljau split, except the
 * handles at a stop, which move to give the piece a defined end tangent.
 *
 * Pure stage: options default here; callers pass the machine's values.
 */

import { bezierDeriv1, bezierDeriv2, bezierPoint, cubic, splitAt, type CubicBezier, type Pt } from "./geometry.js";
import type { CleanSubpath } from "./clean.js";

export interface AnnotateOptions {
    /** A piece is split while κ_max / max(κ_min, kappaFloor) exceeds this. */
    readonly kappaRatio: number;
    /** Curvature (1/mm) below which a piece counts as straight for the ratio. */
    readonly kappaFloor: number;
    /** Largest error of the t(s) fit, in t. */
    readonly fitTol: number;
    /** Halvings allowed for the κ ratio and the fit, each. */
    readonly maxSplitDepth: number;
    /** No inflection, κ-ratio or fit split leaves a piece shorter than this (mm). */
    readonly minLength: number;
}

export const DEFAULT_ANNOTATE_OPTIONS: AnnotateOptions = {
    kappaRatio: 2,
    kappaFloor: 1e-3,
    fitTol: 1e-3,
    maxSplitDepth: 8,
    minLength: 0.05,
};

export const BezierFlag = { START: 1, BREAK: 2, END: 4 } as const;

export interface BezierAnalysis {
    readonly length: number;
    readonly kappaMax: number;
    readonly dkappaMax: number;
    /** t(s) = s·(c1 + s·(c2 + s·c3)). */
    readonly ts: readonly [number, number, number];
    /** Signed; positive turns left. */
    readonly kappaStart: number;
    readonly kappaEnd: number;
    /** Largest |t(s_i) − t_i| over the samples. */
    readonly fitError: number;
}

export interface AnnotatedBezier extends BezierAnalysis {
    readonly curve: CubicBezier;
    readonly flags: number;
}

export type BezierError = "DegenerateHandle" | "Cusp" | "NonMonotonic";

// The Pico's constants (bezier.cpp).
const N = 128;
const HANDLE_TOL = 1e-4;
/** A speed below this fraction of the length is a stop. */
const STOP_FRACTION = 1e-3;
const GX = [-Math.sqrt(0.6), 0, Math.sqrt(0.6)];
const GW = [5 / 9, 8 / 9, 5 / 9];

const norm = (v: Pt) => Math.hypot(v.x, v.y);
const dist = (a: Pt, b: Pt) => Math.hypot(a.x - b.x, a.y - b.y);

function signedCurvature(c: CubicBezier, t: number): number {
    const d = bezierDeriv1(c, t), dd = bezierDeriv2(c, t);
    const n = norm(d);
    return (d.x * dd.y - d.y * dd.x) / (n * n * n);
}

/** bezier.cpp's analyzeBezier in doubles: same samples, same refusals. */
export function analyzeBezier(c: CubicBezier): BezierAnalysis | { error: BezierError } {
    if (dist(c.p1, c.p0) < HANDLE_TOL || dist(c.p3, c.p2) < HANDLE_TOL) return { error: "DegenerateHandle" };

    const s = new Float64Array(N + 1);
    let minSpeed = Infinity;
    for (let i = 0; i < N; i++) {
        const t0 = i / N, h = 1 / N;
        let acc = 0;
        for (let k = 0; k < 3; k++) acc += GW[k]! * norm(bezierDeriv1(c, t0 + h * 0.5 * (GX[k]! + 1)));
        s[i + 1] = s[i]! + acc * h * 0.5;
        minSpeed = Math.min(minSpeed, norm(bezierDeriv1(c, t0)));
    }
    minSpeed = Math.min(minSpeed, norm(bezierDeriv1(c, 1)));
    const L = s[N]!;
    if (minSpeed < STOP_FRACTION * L) return { error: "Cusp" };

    let kPrev = signedCurvature(c, 0);
    const kappaStart = kPrev;
    let kappaMax = Math.abs(kPrev), dkappaMax = 0;
    for (let i = 1; i <= N; i++) {
        const k = signedCurvature(c, i / N);
        kappaMax = Math.max(kappaMax, Math.abs(k));
        dkappaMax = Math.max(dkappaMax, Math.abs(k - kPrev) / (s[i]! - s[i - 1]!));
        kPrev = k;
    }

    // t(u) = u + d2·(u² − u) + d3·(u³ − u), u = s/L: exact at both ends.
    let a22 = 0, a23 = 0, a33 = 0, r2 = 0, r3 = 0;
    for (let i = 1; i < N; i++) {
        const u = s[i]! / L;
        const f2 = u * u - u, f3 = u * u * u - u, r = i / N - u;
        a22 += f2 * f2;
        a23 += f2 * f3;
        a33 += f3 * f3;
        r2 += f2 * r;
        r3 += f3 * r;
    }
    const det = a22 * a33 - a23 * a23;
    const d2 = det !== 0 ? (r2 * a33 - r3 * a23) / det : 0;
    const d3 = det !== 0 ? (a22 * r3 - a23 * r2) / det : 0;

    let slopeMin = Math.min(1 - d2 - d3, 1 + d2 + 2 * d3);
    if (d3 !== 0) {
        const uv = -d2 / (3 * d3);
        if (uv > 0 && uv < 1) slopeMin = Math.min(slopeMin, 1 + d2 * (2 * uv - 1) + d3 * (3 * uv * uv - 1));
    }
    if (!(slopeMin > 0)) return { error: "NonMonotonic" };

    const ts: [number, number, number] = [(1 - d2 - d3) / L, d2 / (L * L), d3 / (L * L * L)];
    let fitError = 0;
    for (let i = 0; i <= N; i++) {
        const si = s[i]!;
        const t = Math.min(1, Math.max(0, si * (ts[0] + si * (ts[1] + si * ts[2]))));
        fitError = Math.max(fitError, Math.abs(t - i / N));
    }
    return { length: L, kappaMax, dkappaMax, ts, kappaStart, kappaEnd: kPrev, fitError };
}

// ── splits ───────────────────────────────────────────────────────────────────

/** Split c at the sorted parameters ts, all in (0, 1). */
function splitMany(c: CubicBezier, ts: number[]): CubicBezier[] {
    const out: CubicBezier[] = [];
    let rest = c, done = 0;
    for (const t of ts) {
        const [a, b] = splitAt(rest, (t - done) / (1 - done));
        out.push(a);
        rest = b;
        done = t;
    }
    out.push(rest);
    return out;
}

function chordLength(c: CubicBezier): number {
    return dist(c.p0, c.p1) + dist(c.p1, c.p2) + dist(c.p2, c.p3);
}

/** Interior parameters where |B'| has a minimum below the Pico's stop threshold. */
function cusps(c: CubicBezier): number[] {
    const M = 2 * N;
    const speed = (t: number) => norm(bezierDeriv1(c, t));
    const v = Array.from({ length: M + 1 }, (_, i) => speed(i / M));
    const limit = STOP_FRACTION * chordLength(c);
    const out: number[] = [];
    for (let i = 1; i < M; i++) {
        if (!(v[i]! <= v[i - 1]! && v[i]! < v[i + 1]!)) continue;
        let lo = (i - 1) / M, hi = (i + 1) / M;
        for (let k = 0; k < 60; k++) {
            const m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
            if (speed(m1) < speed(m2)) hi = m2;
            else lo = m1;
        }
        const t = (lo + hi) / 2;
        if (speed(t) < limit) out.push(t);
    }
    return out;
}

/**
 * Give a stopped end a speed the Pico accepts. A handle with a direction is
 * lengthened along it; a degenerate one moves 1/3 toward the next distinct
 * control point (clean's rule).
 */
function fixStops(c: CubicBezier): CubicBezier {
    const L = chordLength(c);
    const minHandle = (2 * STOP_FRACTION * L) / 3; // twice the Pico's limit, as a handle
    const fix = (end: Pt, h: Pt, q1: Pt, q2: Pt): Pt => {
        const len = dist(h, end);
        if (3 * len >= STOP_FRACTION * L) return h;
        if (len >= HANDLE_TOL) {
            const k = minHandle / len;
            return { x: end.x + (h.x - end.x) * k, y: end.y + (h.y - end.y) * k };
        }
        const q = dist(q1, end) >= HANDLE_TOL ? q1 : q2;
        return { x: end.x + (q.x - end.x) / 3, y: end.y + (q.y - end.y) / 3 };
    };
    const p1 = fix(c.p0, c.p1, c.p2, c.p3);
    const p2 = fix(c.p3, c.p2, c.p1, c.p0);
    return p1 === c.p1 && p2 === c.p2 ? c : cubic(c.p0, p1, p2, c.p3);
}

/** Roots of B' × B'' in (0, 1) that leave no piece shorter than minLength. */
function inflections(c: CubicBezier, minLength: number): number[] {
    // B(t) = p0 + A·t + B·t² + C·t³; B' × B'' = 2A×B + 6A×C·t + 6B×C·t².
    const A = { x: 3 * (c.p1.x - c.p0.x), y: 3 * (c.p1.y - c.p0.y) };
    const B = { x: 3 * (c.p2.x - 2 * c.p1.x + c.p0.x), y: 3 * (c.p2.y - 2 * c.p1.y + c.p0.y) };
    const C = {
        x: c.p3.x - 3 * c.p2.x + 3 * c.p1.x - c.p0.x,
        y: c.p3.y - 3 * c.p2.y + 3 * c.p1.y - c.p0.y,
    };
    const cross = (u: Pt, w: Pt) => u.x * w.y - u.y * w.x;
    const q0 = 2 * cross(A, B), q1 = 6 * cross(A, C), q2 = 6 * cross(B, C);
    const scale = (norm(A) + norm(B) + norm(C)) ** 2;
    const eps = 1e-12 * scale;
    let roots: number[];
    if (Math.abs(q2) > eps) {
        const disc = q1 * q1 - 4 * q2 * q0;
        if (disc < 0) return [];
        const r = Math.sqrt(disc);
        // Numerically stable pair.
        const q = -0.5 * (q1 + Math.sign(q1 || 1) * r);
        roots = [q / q2, q !== 0 ? q0 / q : NaN];
    } else if (Math.abs(q1) > eps) {
        roots = [-q0 / q1];
    } else {
        return [];
    }
    const kept: number[] = [];
    let from = c.p0;
    for (const t of roots.filter((t) => t > 0 && t < 1).sort((a, b) => a - b)) {
        const at = bezierPoint(c, t);
        if (dist(from, at) >= minLength && dist(at, c.p3) >= minLength) {
            kept.push(t);
            from = at;
        }
    }
    return kept;
}

/** Split c at t, or null if either piece would be shorter than minLength. */
function splitIfLong(c: CubicBezier, t: number, o: AnnotateOptions): [CubicBezier, CubicBezier] | null {
    const halves = splitAt(c, t);
    return halves.every((h) => dist(h.p0, h.p3) >= o.minLength) ? halves : null;
}

/** Split while the curvature ratio is above the option; at the κ that balances it. */
function byKappaRatio(c: CubicBezier, o: AnnotateOptions, depth = 0): CubicBezier[] {
    const k = Array.from({ length: N + 1 }, (_, i) => Math.abs(signedCurvature(c, i / N)));
    const kMax = Math.max(...k), kMin = Math.max(Math.min(...k), o.kappaFloor);
    if (kMax / kMin <= o.kappaRatio || depth >= o.maxSplitDepth) return [c];
    const mid = Math.sqrt(kMax * kMin);
    let t = 0.5;
    for (let i = 1; i <= N; i++) {
        if ((k[i - 1]! - mid) * (k[i]! - mid) <= 0) {
            t = i / N;
            break;
        }
    }
    if (t <= 1e-3 || t >= 1 - 1e-3) t = 0.5;
    const halves = splitIfLong(c, t, o);
    if (!halves) return [c];
    return [...byKappaRatio(halves[0], o, depth + 1), ...byKappaRatio(halves[1], o, depth + 1)];
}

/** Halve until the t(s) fit is within fitTol; analyse each piece. */
function byFit(c: CubicBezier, o: AnnotateOptions, depth = 0): { curve: CubicBezier; a: BezierAnalysis }[] {
    const a = analyzeBezier(c);
    const bad = "error" in a ? a.error : a.fitError > o.fitTol ? "fit" : null;
    if (bad === null) return [{ curve: c, a: a as BezierAnalysis }];
    const halves = depth < o.maxSplitDepth ? splitIfLong(c, 0.5, o) : null;
    if ((bad === "NonMonotonic" || bad === "fit") && halves) {
        return [...byFit(halves[0], o, depth + 1), ...byFit(halves[1], o, depth + 1)];
    }
    if (bad === "fit") return [{ curve: c, a: a as BezierAnalysis }];
    throw new Error(`annotate: the Pico would refuse a piece (${bad}) from (${c.p0.x}, ${c.p0.y})`);
}

// ── annotate ─────────────────────────────────────────────────────────────────

export function annotate(sp: CleanSubpath, options: Partial<AnnotateOptions> = {}): AnnotatedBezier[] {
    const o = { ...DEFAULT_ANNOTATE_OPTIONS, ...options };
    const out: AnnotatedBezier[] = [];
    sp.curves.forEach((curve, i) => {
        const corner = i > 0 && sp.joins[i - 1] === "corner";
        splitMany(curve, cusps(curve)).forEach((piece, j) => {
            const fixed = fixStops(piece);
            const pieces = splitMany(fixed, inflections(fixed, o.minLength))
                .flatMap((p) => byKappaRatio(p, o))
                .flatMap((p) => byFit(p, o));
            pieces.forEach(({ curve: c, a }, k) => {
                const brk = k === 0 && (j > 0 || corner);
                out.push({ curve: c, flags: brk ? BezierFlag.BREAK : 0, ...a });
            });
        });
    });
    if (out.length > 0) {
        out[0] = { ...out[0]!, flags: out[0]!.flags | BezierFlag.START };
        const last = out[out.length - 1]!;
        out[out.length - 1] = { ...last, flags: last.flags | BezierFlag.END };
    }
    return out;
}
