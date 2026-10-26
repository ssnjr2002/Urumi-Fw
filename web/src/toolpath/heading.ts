/**
 * heading.ts — a piece's heading and unwrapped turn along it, as the Pico's
 * planner computes them (lib/planner/bezier.cpp bezierHeading, bezierTurn).
 * Same samples, so the host's sweep and the Pico's turn agree to float drift.
 */

import { bezierDeriv1, type CubicBezier } from "./geometry.js";
import type { BezierAnalysis } from "./annotate.js";

type Piece = BezierAnalysis & { readonly curve: CubicBezier };

export interface Turn {
    /** Degrees from the start heading to the end; + counter-clockwise. */
    readonly total: number;
    /** Extremes along the way, degrees from the start heading. */
    readonly lo: number;
    readonly hi: number;
}

/** Heading at arc length `s`, radians; null at a near-stop. */
export function headingAt(b: Piece, s: number): number | null {
    const [c1, c2, c3] = b.ts;
    const t = Math.min(1, Math.max(0, s * (c1 + s * (c2 + s * c3))));
    const d = bezierDeriv1(b.curve, t);
    if (Math.hypot(d.x, d.y) < 1e-4 * b.length) return null;
    return Math.atan2(d.y, d.x);
}

/** Wrapped into (−π, π]. */
export function wrapRad(d: number): number {
    return d - 2 * Math.PI * Math.round(d / (2 * Math.PI));
}

export function bezierTurn(b: Piece): Turn {
    const len = b.length;
    // Each sample turns at most κ_max·Δs: π/4 keeps the unwrap unambiguous.
    const n = Math.min(1024, Math.max(4, Math.ceil(b.kappaMax * len / (Math.PI / 4))));
    let prev = headingAt(b, 0) ?? 0;
    let turn = 0, lo = 0, hi = 0;
    for (let i = 1; i <= n; i++) {
        const h = headingAt(b, len * i / n);
        if (h === null) continue;
        turn += wrapRad(h - prev);
        prev = h;
        lo = Math.min(lo, turn);
        hi = Math.max(hi, turn);
    }
    const deg = 180 / Math.PI;
    return { total: turn * deg, lo: lo * deg, hi: hi * deg };
}
