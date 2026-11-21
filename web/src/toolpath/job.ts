/**
 * job.ts — an SVG to a job's layers: load, offset, clean, annotate, pack.
 * Contours go in document order; each starts with a START record, so the
 * Pico queues the travel to it.
 */

import { readSvg, toolLayer, type Subpath } from "./svg.js";
import type { ControllerConfig, QualityConfig } from "../config/schema.js";
import { packBezier, type Point } from "../packets/bezier.js";
import type { JobLayer, ToolBox } from "../packets/layer.js";
import { cleanSubpath } from "./clean.js";
import { annotate, type AnnotatedBezier } from "./annotate.js";
import { add, cubic, type CubicBezier, type Pt } from "./geometry.js";

/** The most tools one job may use (JOB_HDR's tool count). */
const MAX_TOOLS = 4;

export interface JobSpec {
    /** The material, mm. */
    readonly thickness: number;
    /** Added to every point, mm. */
    readonly offset?: Point;
    /** One entry per job layer, in the order they run. */
    readonly layers: readonly LayerSpec[];
}

export interface LayerSpec {
    /** A tool layer's key (loadSvg). */
    readonly key: string;
    readonly head: number;
    /** mm/s; 0 or absent: the tool's cutFeed. */
    readonly feed?: number;
    /** Plunge below the top, mm; absent: the thickness. */
    readonly depth?: number;
}

export type JobResult =
    | { readonly ok: true; readonly layers: JobLayer[] }
    | { readonly ok: false; readonly errors: readonly string[] };

export interface BezierJobOptions {
    /** Added to every point (machine mm); the drawing's origin lands here. */
    readonly offset?: Pt;
    readonly quality?: Pick<QualityConfig, "angleTol" | "gapTol">;
    /** The config's tool names, so `layer` is a tool layer's key (readSvg). */
    readonly tools?: readonly string[];
}

export interface BezierJob {
    /** One array per contour, in streaming order. */
    readonly contours: AnnotatedBezier[][];
    /** One record per piece, seq 0; the session stamps seq. */
    readonly records: Uint8Array[];
    /** Of the control points, so it contains the cut. */
    readonly bbox: ToolBox;
}

function shift(c: CubicBezier, d: Pt): CubicBezier {
    return cubic(add(c.p0, d), add(c.p1, d), add(c.p2, d), add(c.p3, d));
}

function bboxOf(pieces: AnnotatedBezier[]): ToolBox {
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

/** One layer's subpaths to records; null if nothing to cut. Throws on a piece the Pico would refuse. */
function layerJob(subpaths: readonly Subpath[], options: BezierJobOptions): BezierJob | null {
    const offset = options.offset ?? { x: 0, y: 0 };
    const contours: AnnotatedBezier[][] = [];
    for (const sp of subpaths) {
        const clean = cleanSubpath({ curves: sp.curves.map((c) => shift(c, offset)), closed: sp.closed }, options.quality);
        if (clean) contours.push(annotate(clean));
    }
    if (contours.length === 0) return null;
    const pieces = contours.flat();
    return { contours, records: pieces.map((b) => packBezier(b)), bbox: bboxOf(pieces) };
}

/** One layer's BEZIER records, with no header. */
export function prepareBezierJob(
    svgText: string,
    layer: string,
    options: BezierJobOptions = {},
): BezierJob | { error: string } {
    const subpaths = readSvg(svgText, { tools: options.tools }).layers.get(layer);
    if (!subpaths) return { error: `no layer "${layer}"` };
    return layerJob(subpaths, options) ?? { error: `layer "${layer}" has nothing to cut` };
}

const message = (e: unknown): string => (e instanceof Error ? e.message : String(e));

/** A job's layers in `spec.layers`' order, or every error found. */
export function buildJob(svg: string, config: ControllerConfig, spec: JobSpec): JobResult {
    const tools = Object.keys(config.toolProfiles);
    let read;
    try {
        read = readSvg(svg, { tools });
    } catch (e) {
        return { ok: false, errors: [`svg: ${message(e)}`] };
    }

    const errors: string[] = [];
    const layers: JobLayer[] = [];
    const heads = new Map<number, { key: string; head: number }>();
    const options = { offset: spec.offset, quality: config.quality };

    spec.layers.forEach((l, i) => {
        const subpaths = l.key === "" ? undefined : read.layers.get(l.key);
        const tl = subpaths && toolLayer(l.key, tools);
        if (!subpaths || !tl) return void errors.push(`${l.key}: not a tool layer of the SVG`);
        const profile = config.toolProfiles[tl.tool]!;
        if (profile.id == null) return void errors.push(`${l.key}: tool ${tl.tool} has no id`);

        const chambers = profile.chamberOffsets?.length ?? 0;
        if (chambers ? !(tl.chamber >= 1 && tl.chamber <= chambers) : tl.chamber !== 0) {
            errors.push(chambers ? `${l.key}: chamber ${tl.chamber} not in 1..${chambers}` : `${l.key}: tool ${tl.tool} has no chambers`);
        }
        const depth = l.depth ?? spec.thickness;
        if (profile.plunge && !(depth > 0)) errors.push(`${l.key}: tool ${tl.tool} plunges, so needs a depth above 0`);
        const other = heads.get(profile.id);
        if (!other) heads.set(profile.id, { key: l.key, head: l.head });
        else if (other.head !== l.head) {
            errors.push(`${other.key} and ${l.key}: tool ${tl.tool} on heads ${other.head} and ${l.head}`);
        }

        let job;
        try {
            job = layerJob(subpaths, options);
        } catch (e) {
            return void errors.push(`${l.key}: ${message(e)}`);
        }
        if (!job) return void errors.push(`${l.key}: nothing to cut`);
        layers.push({
            hdr: { layer: i, record: 0, chamber: tl.chamber, feed: l.feed ?? 0, p: depth, thickness: spec.thickness },
            tool: profile.id,
            head: l.head,
            bbox: job.bbox,
            records: job.records,
        });
    });

    if (spec.layers.length === 0) errors.push("no layers");
    if (heads.size > MAX_TOOLS) errors.push(`${heads.size} tools, at most ${MAX_TOOLS}`);
    return errors.length ? { ok: false, errors } : { ok: true, layers };
}
