/**
 * Stage 2: SVG pixel coordinates -> millimetres + Y-axis flip.
 * Ported from host/production/normalise.py.
 *
 * Reads viewBox and width/height from the SVG root, builds a transform,
 * applies it to every control point from stage 1.
 * Output: same CubicBezier list, coordinates in mm, machine origin at bottom-left.
 *
 * Like parse.ts, the loadSvg* functions take SVG text (not a file path).
 */

import type { CubicBezier, Pt } from "./bezier.js";
import { cubic } from "./bezier.js";
import { loadSvg, loadSvgSubpaths, loadSvgLayers, parseSvgRoot } from "./parse.js";

// ── unit conversion to mm ─────────────────────────────────────────────────────

const UNIT_TO_MM: Readonly<Record<string, number>> = {
    mm: 1.0,
    cm: 10.0,
    in: 25.4,
    pt: 25.4 / 72,
    pc: 25.4 / 6,
    px: 25.4 / 96,
    "": 25.4 / 96, // unitless treated as px
};

const DIM_RE = /^\s*([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*(mm|cm|in|pt|pc|px)?\s*$/;

function toMm(valueStr: string): number {
    const m = valueStr.match(DIM_RE);
    if (!m) throw new Error(`Cannot parse dimension: '${valueStr}'`);
    const val = parseFloat(m[1]!);
    const unit = (m[2] ?? "").toLowerCase();
    const factor = UNIT_TO_MM[unit];
    if (factor === undefined) throw new Error(`Unknown unit: '${unit}'`);
    return val * factor;
}

// ── viewport parsing ──────────────────────────────────────────────────────────

export interface Viewport {
    readonly vbMinX: number;
    readonly vbMinY: number;
    readonly vbW: number;
    readonly vbH: number;
    readonly widthMm: number;
    readonly heightMm: number;
}

/**
 * Parse viewBox + width/height from the SVG root.
 * Falls back to viewBox px == mm when width/height are absent.
 */
export function parseViewport(svgText: string): Viewport {
    const root = parseSvgRoot(svgText);

    const vbAttr = (root.getAttribute("viewBox") ?? "").trim();
    let vbMinX: number, vbMinY: number, vbW: number, vbH: number;
    if (vbAttr) {
        const parts = vbAttr.split(/\s+/).map(parseFloat);
        if (parts.length !== 4) throw new Error(`Invalid viewBox: '${vbAttr}'`);
        vbMinX = parts[0]!;
        vbMinY = parts[1]!;
        vbW = parts[2]!;
        vbH = parts[3]!;
    } else {
        vbMinX = 0;
        vbMinY = 0;
        vbW = parseFloat(root.getAttribute("width") ?? "100");
        vbH = parseFloat(root.getAttribute("height") ?? "100");
    }

    const wAttr = root.getAttribute("width");
    const hAttr = root.getAttribute("height");

    let widthMm: number, heightMm: number;
    if (wAttr && hAttr) {
        widthMm = toMm(wAttr);
        heightMm = toMm(hAttr);
    } else {
        // no physical size declared — treat viewBox units as mm 1:1
        widthMm = vbW;
        heightMm = vbH;
    }

    return { vbMinX, vbMinY, vbW, vbH, widthMm, heightMm };
}

// ── coordinate transform ──────────────────────────────────────────────────────

/**
 * Build a transform function pt_svg -> pt_mm that applies:
 *   1. viewBox offset (subtract minX, minY)
 *   2. scale to mm
 *   3. Y-axis flip (SVG +Y down -> machine +Y up)
 */
export function makeTransform(vp: Viewport): (pt: Pt) => Pt {
    const sx = vp.widthMm / vp.vbW;
    const sy = vp.heightMm / vp.vbH;
    return (pt: Pt): Pt => ({
        x: (pt.x - vp.vbMinX) * sx,
        y: vp.heightMm - (pt.y - vp.vbMinY) * sy,
    });
}

export function applyTransform(curves: readonly CubicBezier[], transform: (pt: Pt) => Pt): CubicBezier[] {
    return curves.map((c) => cubic(transform(c.p0), transform(c.p1), transform(c.p2), transform(c.p3)));
}

// ── public entry points ───────────────────────────────────────────────────────

/** Full stage 1+2: SVG text -> cubic Beziers in mm (flat list). */
export function loadSvgMm(svgText: string): { curves: CubicBezier[]; viewport: Viewport } {
    const curvesPx = loadSvg(svgText);
    const viewport = parseViewport(svgText);
    const transform = makeTransform(viewport);
    return { curves: applyTransform(curvesPx, transform), viewport };
}

/** Full stage 1+2: SVG text -> list[list[CubicBezier]] in mm. */
export function loadSvgMmSubpaths(svgText: string): { subpaths: CubicBezier[][]; viewport: Viewport } {
    const subpathsPx = loadSvgSubpaths(svgText);
    const viewport = parseViewport(svgText);
    const transform = makeTransform(viewport);
    return {
        subpaths: subpathsPx.map((sp) => applyTransform(sp, transform)),
        viewport,
    };
}

/** Layer-aware stage 1+2: SVG text -> Map { layer -> list[subpath] } in mm. */
export function loadSvgMmLayers(svgText: string): { layers: Map<string, CubicBezier[][]>; viewport: Viewport } {
    const layersPx = loadSvgLayers(svgText);
    const viewport = parseViewport(svgText);
    const transform = makeTransform(viewport);
    const layersMm = new Map<string, CubicBezier[][]>();
    for (const [name, subs] of layersPx) {
        layersMm.set(name, subs.map((sp) => applyTransform(sp, transform)));
    }
    return { layers: layersMm, viewport };
}
