/**
 * ingest.ts — SVG ingestion: stages 1 (parse) + 2 (normalise).
 *
 * Merged from host/production/parse.py + normalise.py. The two stages are
 * kept as clearly-delineated sections because they are conceptually distinct
 * (parse produces SVG-pixel cubics; normalise maps them to mm + Y-flip) but
 * share the XML parse — loadSvgMm* walk the SVG root once for both curve
 * extraction and viewport reading, avoiding the double-parse that separate
 * files would force.
 *
 * All loadSvg* functions take SVG text (not a file path) — the caller is
 * responsible for reading the file (fetch, FileReader, etc.). XML parsing
 * uses the standard DOMParser API (native in browsers; polyfilled in Node
 * tests via @xmldom/xmldom setup in test-setup.ts).
 *
 * Curve primitives (Pt, CubicBezier, cubic, KAPPA, lineToCubic, quadToCubic)
 * live in ../toolpath/geometry.ts — they are geometric entities, not SVG
 * concepts, and the toolpath stages (3+) consume them too.
 */

import {
    KAPPA,
    cubic,
    lineToCubic,
    quadToCubic,
    type CubicBezier,
    type Pt,
} from "../toolpath/src/geometry.js";

// ──────────────────────────────────────────────────────────────────────────────
// Stage 1 — SVG path -> list of cubic Beziers (SVG pixel coordinates)
// Handles M, L, H, V, C, S, Q, Z (absolute and relative); <circle>, <ellipse>,
// <rect>, <line>, <polygon>, <polyline> elements.
// ──────────────────────────────────────────────────────────────────────────────

// ── path tokenizer ────────────────────────────────────────────────────────────

const CMD_RE = /([MmLlHhVvCcSsQqZz])|([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)/g;

function tokenize(d: string): (string | number)[] {
    const tokens: (string | number)[] = [];
    for (const m of d.matchAll(CMD_RE)) {
        if (m[1] !== undefined) {
            tokens.push(m[1]);
        } else if (m[2] !== undefined) {
            tokens.push(parseFloat(m[2]));
        }
    }
    return tokens;
}

function num(arr: readonly number[], i: number): number {
    const v = arr[i];
    if (v === undefined) throw new Error(`unexpected end of path data at index ${i}`);
    return v;
}

// ── path string parser ────────────────────────────────────────────────────────

/**
 * Parse an SVG path d attribute and return list[list[CubicBezier]].
 * Each M/m command that is not the first starts a new subpath.
 * cur is preserved across subpath boundaries so relative m works correctly.
 */
export function pathToSubpaths(d: string): CubicBezier[][] {
    const tokens = tokenize(d);
    const nums: number[] = [];
    const cmds: [string, number][] = [];
    for (const t of tokens) {
        if (typeof t === "string") {
            cmds.push([t, nums.length]);
        } else {
            nums.push(t);
        }
    }
    cmds.push(["__end__", nums.length]);

    const subpaths: CubicBezier[][] = [];
    let current: CubicBezier[] = [];
    let cur: Pt = { x: 0, y: 0 };
    let start: Pt = { x: 0, y: 0 };
    let lastCp: Pt | null = null;
    let lastCmd: string | null = null;
    let firstCmd = true;

    for (let ci = 0; ci < cmds.length - 1; ci++) {
        const cmd = cmds[ci]![0];
        const ni = cmds[ci]![1];
        const nextNi = cmds[ci + 1]![1];
        const chunk = nums.slice(ni, nextNi);
        const isRel = cmd.toLowerCase() === cmd;
        let C = cmd.toUpperCase();

        // New subpath boundary: M/m after the very first command
        if (C === "M" && !firstCmd) {
            if (current.length > 0) subpaths.push(current);
            current = [];
        }

        let i = 0;
        while (i < chunk.length || C === "Z") {
            if (C === "M") {
                const x = num(chunk, i), y = num(chunk, i + 1); i += 2;
                cur = isRel ? { x: cur.x + x, y: cur.y + y } : { x, y };
                start = cur;
                lastCp = null;
                C = "L";
                if (i >= chunk.length) break;
                continue;
            }
            if (C === "L") {
                const x = num(chunk, i), y = num(chunk, i + 1); i += 2;
                const p1 = isRel ? { x: cur.x + x, y: cur.y + y } : { x, y };
                current.push(lineToCubic(cur, p1));
                cur = p1; lastCp = null;
                continue;
            }
            if (C === "H") {
                const x = num(chunk, i); i += 1;
                const p1 = isRel ? { x: cur.x + x, y: cur.y } : { x, y: cur.y };
                current.push(lineToCubic(cur, p1));
                cur = p1; lastCp = null;
                continue;
            }
            if (C === "V") {
                const y = num(chunk, i); i += 1;
                const p1 = isRel ? { x: cur.x, y: cur.y + y } : { x: cur.x, y };
                current.push(lineToCubic(cur, p1));
                cur = p1; lastCp = null;
                continue;
            }
            if (C === "C") {
                const x1 = num(chunk, i), y1 = num(chunk, i + 1);
                const x2 = num(chunk, i + 2), y2 = num(chunk, i + 3);
                const x = num(chunk, i + 4), y = num(chunk, i + 5);
                i += 6;
                const p1 = isRel ? { x: cur.x + x1, y: cur.y + y1 } : { x: x1, y: y1 };
                const p2 = isRel ? { x: cur.x + x2, y: cur.y + y2 } : { x: x2, y: y2 };
                const p3 = isRel ? { x: cur.x + x, y: cur.y + y } : { x, y };
                current.push(cubic(cur, p1, p2, p3));
                lastCp = p2; cur = p3;
                continue;
            }
            if (C === "S") {
                const x2 = num(chunk, i), y2 = num(chunk, i + 1);
                const x = num(chunk, i + 2), y = num(chunk, i + 3);
                i += 4;
                const p1 = (lastCmd && "CcSs".includes(lastCmd) && lastCp)
                    ? { x: 2 * cur.x - lastCp.x, y: 2 * cur.y - lastCp.y }
                    : cur;
                const p2 = isRel ? { x: cur.x + x2, y: cur.y + y2 } : { x: x2, y: y2 };
                const p3 = isRel ? { x: cur.x + x, y: cur.y + y } : { x, y };
                current.push(cubic(cur, p1, p2, p3));
                lastCp = p2; cur = p3;
                continue;
            }
            if (C === "Q") {
                const x1 = num(chunk, i), y1 = num(chunk, i + 1);
                const x = num(chunk, i + 2), y = num(chunk, i + 3);
                i += 4;
                const qp1 = isRel ? { x: cur.x + x1, y: cur.y + y1 } : { x: x1, y: y1 };
                const p2 = isRel ? { x: cur.x + x, y: cur.y + y } : { x, y };
                current.push(quadToCubic(cur, qp1, p2));
                lastCp = qp1; cur = p2;
                continue;
            }
            if (C === "Z") {
                if (cur.x !== start.x || cur.y !== start.y) {
                    current.push(lineToCubic(cur, start));
                }
                cur = start; lastCp = null;
                break;
            }
            // Unknown command — stop processing this chunk
            break;
        }

        lastCmd = cmd;
        firstCmd = false;
    }

    if (current.length > 0) subpaths.push(current);
    return subpaths;
}

/** Flat list of CubicBeziers from an SVG path d attribute. */
export function pathToCubics(d: string): CubicBezier[] {
    return pathToSubpaths(d).flat();
}

// ── shape primitives ──────────────────────────────────────────────────────────

/** Approximate an ellipse (or circle when rx==ry) with 4 cubic Beziers. */
export function circleToCubics(cx: number, cy: number, rx: number, ry: number): CubicBezier[] {
    const kx = rx * KAPPA;
    const ky = ry * KAPPA;
    return [
        cubic({ x: cx + rx, y: cy },     { x: cx + rx, y: cy + ky }, { x: cx + kx, y: cy + ry }, { x: cx, y: cy + ry }),
        cubic({ x: cx, y: cy + ry },     { x: cx - kx, y: cy + ry }, { x: cx - rx, y: cy + ky }, { x: cx - rx, y: cy }),
        cubic({ x: cx - rx, y: cy },     { x: cx - rx, y: cy - ky }, { x: cx - kx, y: cy - ry }, { x: cx, y: cy - ry }),
        cubic({ x: cx, y: cy - ry },     { x: cx + kx, y: cy - ry }, { x: cx + rx, y: cy - ky }, { x: cx + rx, y: cy }),
    ];
}

/** Convert a rect (optionally rounded) to cubic Beziers. */
export function rectToCubics(x: number, y: number, w: number, h: number, rx = 0, ry = 0): CubicBezier[] {
    if (rx === 0 && ry === 0) {
        const corners: Pt[] = [
            { x, y },         { x: x + w, y },
            { x: x + w, y: y + h }, { x, y: y + h },
        ];
        return corners.map((c, i) => lineToCubic(c, corners[(i + 1) % 4]!));
    }
    rx = Math.min(rx, w / 2);
    ry = Math.min(ry, h / 2);
    const kx = rx * KAPPA;
    const ky = ry * KAPPA;
    return pathToCubics(
        `M ${x + rx},${y} ` +
        `H ${x + w - rx} C ${x + w - rx + kx},${y} ${x + w},${y + ky} ${x + w},${y + ry} ` +
        `V ${y + h - ry} C ${x + w},${y + h - ry + ky} ${x + w - rx + kx},${y + h} ${x + w - rx},${y + h} ` +
        `H ${x + rx} C ${x + rx - kx},${y + h} ${x},${y + h - ry + ky} ${x},${y + h - ry} ` +
        `V ${y + ry} C ${x},${y + ry - ky} ${x + rx - kx},${y} ${x + rx},${y} Z`,
    );
}

// ── SVG document parsing ──────────────────────────────────────────────────────

const INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape";

const NO_PAINT = ["none", "transparent"];
const DRAWABLE_TAGS = ["path", "circle", "ellipse", "rect", "line", "polygon", "polyline"];

function getFloat(el: Element, attr: string, def = 0): number {
    const v = el.getAttribute(attr);
    return v !== null ? parseFloat(v) : def;
}

/** Resolve a paint property from the element's `style=` declaration first,
 * then its presentation attribute. Returns a lowercased string or null. */
function getStyleProp(el: Element, prop: string): string | null {
    const style = el.getAttribute("style") ?? "";
    for (const decl of style.split(";")) {
        const idx = decl.indexOf(":");
        if (idx >= 0) {
            const k = decl.slice(0, idx).trim();
            if (k === prop) {
                return decl.slice(idx + 1).trim().toLowerCase();
            }
        }
    }
    const val = el.getAttribute(prop);
    return val ? val.trim().toLowerCase() : null;
}

/** True if the element would draw anything — has a visible fill or stroke. */
function isPaintable(el: Element): boolean {
    const fill = getStyleProp(el, "fill");
    const stroke = getStyleProp(el, "stroke");
    if (fill === null || !NO_PAINT.includes(fill)) return true;
    return stroke !== null && !NO_PAINT.includes(stroke);
}

/** Subpaths for a single SVG element: list[list[CubicBezier]]. Empty for
 * non-drawable tags, non-paintable geometry, and degenerate shapes. */
function elementSubpaths(el: Element): CubicBezier[][] {
    const tag = el.localName;
    if (!tag || !DRAWABLE_TAGS.includes(tag)) return [];
    if (!isPaintable(el)) return [];

    if (tag === "path") {
        const d = el.getAttribute("d") ?? "";
        return d ? pathToSubpaths(d) : [];
    }

    if (tag === "circle" || tag === "ellipse") {
        const cx = getFloat(el, "cx");
        const cy = getFloat(el, "cy");
        let rx: number, ry: number;
        if (tag === "circle") {
            const r = getFloat(el, "r");
            rx = ry = r;
        } else {
            rx = getFloat(el, "rx");
            ry = getFloat(el, "ry");
        }
        return (rx > 0 && ry > 0) ? [circleToCubics(cx, cy, rx, ry)] : [];
    }

    if (tag === "rect") {
        const x = getFloat(el, "x");
        const y = getFloat(el, "y");
        const w = getFloat(el, "width");
        const h = getFloat(el, "height");
        const rx = getFloat(el, "rx");
        const ry = getFloat(el, "ry") || rx;
        return (w > 0 && h > 0) ? [rectToCubics(x, y, w, h, rx, ry)] : [];
    }

    if (tag === "line") {
        const x1 = getFloat(el, "x1");
        const y1 = getFloat(el, "y1");
        const x2 = getFloat(el, "x2");
        const y2 = getFloat(el, "y2");
        return [[lineToCubic({ x: x1, y: y1 }, { x: x2, y: y2 })]];
    }

    if (tag === "polygon" || tag === "polyline") {
        const ptsStr = (el.getAttribute("points") ?? "").trim();
        if (!ptsStr) return [];
        const coords = ptsStr.split(/[\s,]+/).filter((v) => v).map(parseFloat);
        const pts: Pt[] = [];
        for (let i = 0; i < coords.length - 1; i += 2) {
            pts.push({ x: coords[i]!, y: coords[i + 1]! });
        }
        if (pts.length < 2) return [];
        const segs = pts.slice(0, -1).map((p, idx) => lineToCubic(p, pts[idx + 1]!));
        if (tag === "polygon") {
            segs.push(lineToCubic(pts[pts.length - 1]!, pts[0]!));
        }
        return [segs];
    }

    return [];
}

/** The layer name of a <g>: inkscape:label, else id, else null. */
function layerLabel(el: Element): string | null {
    return el.getAttributeNS(INKSCAPE_NS, "label") || el.getAttribute("id");
}

/** Parse SVG text into a root Element. Throws if not a valid SVG document. */
function parseSvgRoot(svgText: string): Element {
    const doc = new DOMParser().parseFromString(svgText, "image/svg+xml");
    const root = doc.documentElement;
    if (!root || root.localName !== "svg") {
        throw new Error("not a valid SVG document");
    }
    return root;
}

/** Returns a flat list of CubicBeziers from all elements in the SVG. */
export function loadSvg(svgText: string): CubicBezier[] {
    return loadSvgSubpaths(svgText).flat();
}

/**
 * Returns list[list[CubicBezier]], one inner list per subpath, in document
 * order across ALL layers (layer-agnostic — the single-tool path).
 * Each SVG primitive element is one subpath; <path> elements are split on M.
 */
export function loadSvgSubpaths(svgText: string): CubicBezier[][] {
    const root = parseSvgRoot(svgText);
    return subpathsFromRoot(root);
}

/**
 * Group subpaths by the layer they live in: returns an ordered
 * Map {layer_name -> list[subpath]}, keyed by the nearest ancestor <g>'s
 * inkscape:label (else id). Geometry with no named-group ancestor goes under
 * the key '' (the default layer). Insertion order follows first appearance.
 */
export function loadSvgLayers(svgText: string): Map<string, CubicBezier[][]> {
    const root = parseSvgRoot(svgText);
    const layers = new Map<string, CubicBezier[][]>();

    function visit(el: Element, layer: string): void {
        const tag = el.localName;
        if (tag === "g") {
            const label = layerLabel(el);
            const childLayer = label !== null ? label : layer;
            for (const child of Array.from(el.children)) {
                visit(child, childLayer);
            }
            return;
        }
        const subs = elementSubpaths(el);
        if (subs.length > 0) {
            const list = layers.get(layer);
            if (list) {
                list.push(...subs);
            } else {
                layers.set(layer, [...subs]);
            }
        }
    }

    for (const child of Array.from(root.children)) {
        visit(child, "");
    }
    return layers;
}

/** Walk a root Element and collect all subpaths in document order. */
function subpathsFromRoot(root: Element): CubicBezier[][] {
    const allSubpaths: CubicBezier[][] = [];
    function walk(el: Element): void {
        const subs = elementSubpaths(el);
        if (subs.length > 0) allSubpaths.push(...subs);
        for (const child of Array.from(el.children)) {
            walk(child);
        }
    }
    walk(root);
    return allSubpaths;
}

// ──────────────────────────────────────────────────────────────────────────────
// Stage 2 — SVG pixel coordinates -> millimetres + Y-axis flip
// Reads viewBox + width/height from the SVG root, builds a transform, applies
// it to every control point from stage 1. Output: mm, machine origin at
// bottom-left (SVG +Y down -> machine +Y up).
// ──────────────────────────────────────────────────────────────────────────────

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

/** Read viewBox + width/height from an already-parsed SVG root Element.
 * Falls back to viewBox px == mm when width/height are absent. */
function viewportFromRoot(root: Element): Viewport {
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

/** Parse SVG text and return its viewport. */
export function parseViewport(svgText: string): Viewport {
    return viewportFromRoot(parseSvgRoot(svgText));
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

// ── public entry points (stage 1+2 combined, single XML parse) ───────────────

/** Full stage 1+2: SVG text -> cubic Beziers in mm (flat list). */
export function loadSvgMm(svgText: string): { curves: CubicBezier[]; viewport: Viewport } {
    const root = parseSvgRoot(svgText);
    const viewport = viewportFromRoot(root);
    const transform = makeTransform(viewport);
    return { curves: applyTransform(subpathsFromRoot(root).flat(), transform), viewport };
}

/** Full stage 1+2: SVG text -> list[list[CubicBezier]] in mm. */
export function loadSvgMmSubpaths(svgText: string): { subpaths: CubicBezier[][]; viewport: Viewport } {
    const root = parseSvgRoot(svgText);
    const viewport = viewportFromRoot(root);
    const transform = makeTransform(viewport);
    return {
        subpaths: subpathsFromRoot(root).map((sp) => applyTransform(sp, transform)),
        viewport,
    };
}

/** Layer-aware stage 1+2: SVG text -> Map { layer -> list[subpath] } in mm. */
export function loadSvgMmLayers(svgText: string): { layers: Map<string, CubicBezier[][]>; viewport: Viewport } {
    const root = parseSvgRoot(svgText);
    const viewport = viewportFromRoot(root);
    const transform = makeTransform(viewport);
    const layersPx = new Map<string, CubicBezier[][]>();

    function visit(el: Element, layer: string): void {
        const tag = el.localName;
        if (tag === "g") {
            const label = layerLabel(el);
            const childLayer = label !== null ? label : layer;
            for (const child of Array.from(el.children)) {
                visit(child, childLayer);
            }
            return;
        }
        const subs = elementSubpaths(el);
        if (subs.length > 0) {
            const list = layersPx.get(layer);
            if (list) {
                list.push(...subs);
            } else {
                layersPx.set(layer, [...subs]);
            }
        }
    }

    for (const child of Array.from(root.children)) {
        visit(child, "");
    }

    const layersMm = new Map<string, CubicBezier[][]>();
    for (const [name, subs] of layersPx) {
        layersMm.set(name, subs.map((sp) => applyTransform(sp, transform)));
    }
    return { layers: layersMm, viewport };
}
