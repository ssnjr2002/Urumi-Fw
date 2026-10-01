/**
 * clean.ts — stage 3 of the Bézier path: make each subpath safe to analyse.
 *
 * In order: drop zero-length curves, give every curve defined end tangents,
 * snap the closure, then classify each join as smooth or corner. Nothing is
 * invented: a curve only moves by a handle fix or a snap under `gapTol`.
 *
 * Pure stage: options default here; callers pass the machine's values.
 */

import {
    add,
    angleBetweenDeg,
    cubic,
    entryTangent,
    exitTangent,
    length,
    lineToCubic,
    scale,
    sub,
    type CubicBezier,
    type Pt,
} from "./geometry.js";

export interface CleanOptions {
    /** A join turning more than this (degrees) is a corner. */
    readonly angleTol: number;
    /** A subpath ending this close (mm) to its start is closed onto it. */
    readonly gapTol: number;
    /** A handle shorter than this (mm) is degenerate; the Pico's threshold. */
    readonly handleTol: number;
}

export const DEFAULT_CLEAN_OPTIONS: CleanOptions = { angleTol: 5, gapTol: 0.01, handleTol: 1e-4 };

export type JoinKind = "smooth" | "corner";

export interface CleanSubpath {
    readonly curves: CubicBezier[];
    /** joins[i] is between curves[i] and the next; a closed subpath's last join wraps to curves[0]. */
    readonly joins: JoinKind[];
    readonly closed: boolean;
}

function dist(a: Pt, b: Pt): number {
    return length(sub(b, a));
}

/** p moved a third of the way to the first of `targets` farther than tol. */
function third(p: Pt, targets: Pt[], tol: number): Pt {
    const t = targets.find((q) => dist(p, q) > tol) ?? p;
    return add(p, scale(sub(t, p), 1 / 3));
}

/** Moves a handle lying on its endpoint toward the next distinct control point. */
function fixHandles(c: CubicBezier, tol: number): CubicBezier {
    const p1 = dist(c.p0, c.p1) > tol ? c.p1 : third(c.p0, [c.p2, c.p3], tol);
    const p2 = dist(c.p3, c.p2) > tol ? c.p2 : third(c.p3, [c.p1, c.p0], tol);
    return cubic(c.p0, p1, p2, c.p3);
}

/** Moves a curve's end to `p`, carrying its last handle along. */
function moveEnd(c: CubicBezier, p: Pt): CubicBezier {
    return cubic(c.p0, c.p1, add(c.p2, sub(p, c.p3)), p);
}

function joinKind(a: CubicBezier, b: CubicBezier, angleTol: number): JoinKind {
    return angleBetweenDeg(exitTangent(a), entryTangent(b)) > angleTol ? "corner" : "smooth";
}

/** Cleans one subpath; null if nothing of it is left. */
export function cleanSubpath(
    sp: { readonly curves: readonly CubicBezier[]; readonly closed: boolean },
    options: Partial<CleanOptions> = {},
): CleanSubpath | null {
    const { angleTol, gapTol, handleTol } = { ...DEFAULT_CLEAN_OPTIONS, ...options };

    const curves: CubicBezier[] = [];
    for (const c of sp.curves) {
        const zeroLength = [c.p1, c.p2, c.p3].every((p) => dist(c.p0, p) <= handleTol);
        if (zeroLength) continue;
        const prev = curves[curves.length - 1];
        // A dropped curve leaves a gap of at most handleTol; close it.
        const fixed = fixHandles(prev ? cubic(prev.p3, c.p1, c.p2, c.p3) : c, handleTol);
        curves.push(fixed);
    }
    if (curves.length === 0) return null;

    const first = curves[0]!;
    const last = curves[curves.length - 1]!;
    let closed = sp.closed;
    if (dist(last.p3, first.p0) <= gapTol) {
        curves[curves.length - 1] = moveEnd(last, first.p0);
        closed = true;
    } else if (closed) {
        // Only when the loader's zTol is above gapTol.
        curves.push(lineToCubic(last.p3, first.p0));
    }

    const joins: JoinKind[] = [];
    for (let i = 0; i + 1 < curves.length; i++) joins.push(joinKind(curves[i]!, curves[i + 1]!, angleTol));
    if (closed) joins.push(joinKind(curves[curves.length - 1]!, first, angleTol));
    return { curves, joins, closed };
}
