/**
 * job.ts — one SVG layer to BEZIER packets: load, offset, clean, annotate,
 * pack. Contours go in document order; each starts with a START record, so the
 * Pico queues the travel to it.
 */

import { loadSvgPaths } from "../svg/load.js";
import type { QualityConfig } from "../machine/schema.js";
import { packBezier } from "../wire/format/bezier.js";
import { cleanSubpath } from "./clean.js";
import { annotate, type AnnotatedBezier } from "./annotate.js";
import { add, cubic, type CubicBezier, type Pt } from "./geometry.js";

export interface BezierJobOptions {
    /** Added to every point (machine mm); the drawing's origin lands here. */
    readonly offset?: Pt;
    readonly quality?: Pick<QualityConfig, "angleTol" | "gapTol">;
}

export interface BBox {
    readonly minX: number;
    readonly minY: number;
    readonly maxX: number;
    readonly maxY: number;
}

export interface BezierJob {
    /** One array per contour, in streaming order. */
    readonly contours: AnnotatedBezier[][];
    /** One packet per piece, seq 0; the session stamps seq. */
    readonly packets: Uint8Array[];
    /** Of the control points, so it contains the cut. */
    readonly bbox: BBox;
}

function shift(c: CubicBezier, d: Pt): CubicBezier {
    return cubic(add(c.p0, d), add(c.p1, d), add(c.p2, d), add(c.p3, d));
}

function bboxOf(pieces: AnnotatedBezier[]): BBox {
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (const { curve } of pieces) {
        for (const p of [curve.p0, curve.p1, curve.p2, curve.p3]) {
            minX = Math.min(minX, p.x);
            minY = Math.min(minY, p.y);
            maxX = Math.max(maxX, p.x);
            maxY = Math.max(maxY, p.y);
        }
    }
    return { minX, minY, maxX, maxY };
}

export function prepareBezierJob(
    svgText: string,
    layer: string,
    options: BezierJobOptions = {},
): BezierJob | { error: string } {
    const subpaths = loadSvgPaths(svgText).layers.get(layer);
    if (!subpaths) return { error: `no layer "${layer}"` };
    const offset = options.offset ?? { x: 0, y: 0 };

    const contours: AnnotatedBezier[][] = [];
    for (const sp of subpaths) {
        const clean = cleanSubpath({ curves: sp.curves.map((c) => shift(c, offset)), closed: sp.closed }, options.quality);
        if (clean) contours.push(annotate(clean));
    }
    if (contours.length === 0) return { error: `layer "${layer}" has nothing to cut` };

    const pieces = contours.flat();
    return { contours, packets: pieces.map((b) => packBezier(b)), bbox: bboxOf(pieces) };
}
