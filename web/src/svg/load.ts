/**
 * load.ts — SVG to subpaths in mm (Y up), stages 1-2 of the Bézier path.
 *
 * One walk of the document carries the transform matrix (element × parents ×
 * viewBox-to-mm) and the layer name. Each element is parsed to cubics in its
 * own coordinates, then mapped by the matrix, which is exact for cubics.
 *
 * Layers are named as in ingest.ts: a <g>'s inkscape:label (else id), nested
 * groups joined by '/', geometry outside any named group under ''.
 */

import {
    IDENTITY,
    KAPPA,
    applyCubic,
    arcToCubics,
    compose,
    cubic,
    length,
    lineToCubic,
    quadToCubic,
    sub,
    type Affine,
    type CubicBezier,
    type Pt,
} from "../toolpath/geometry.js";

export interface Subpath {
    readonly curves: CubicBezier[];
    /** Ended by Z, or a closed shape (circle, ellipse, rect, polygon). */
    readonly closed: boolean;
}

export interface Viewport {
    readonly vbMinX: number;
    readonly vbMinY: number;
    readonly vbW: number;
    readonly vbH: number;
    readonly widthMm: number;
    readonly heightMm: number;
}

export interface LoadedSvg {
    readonly layers: Map<string, Subpath[]>;
    readonly viewport: Viewport;
}

export interface LoadOptions {
    /** A Z closing line shorter than this (mm) is dropped. */
    readonly zTol?: number;
}

const DEFAULT_Z_TOL = 1e-6;

/** A subpath in element coordinates; `zLine` marks a last curve added by Z. */
interface LocalSubpath {
    curves: CubicBezier[];
    closed: boolean;
    zLine: boolean;
}

// ── path data ─────────────────────────────────────────────────────────────────

const NUM_RE = /[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?/y;
const ARGS: Readonly<Record<string, number>> = { M: 2, L: 2, H: 1, V: 1, C: 6, S: 4, Q: 4, T: 2, A: 7, Z: 0 };

class PathReader {
    private i = 0;
    constructor(private readonly d: string) {}

    private skip(): void {
        while (this.i < this.d.length && /[\s,]/.test(this.d[this.i]!)) this.i++;
    }

    done(): boolean {
        this.skip();
        return this.i >= this.d.length;
    }

    /** The next command letter, or null when a number follows (implicit repeat). */
    command(): string | null {
        this.skip();
        const ch = this.d[this.i]!;
        if (/[0-9+\-.]/.test(ch)) return null;
        if (!(ch.toUpperCase() in ARGS)) throw new Error(`unknown path command '${ch}' at ${this.i}`);
        this.i++;
        return ch;
    }

    number(): number {
        this.skip();
        NUM_RE.lastIndex = this.i;
        const m = NUM_RE.exec(this.d);
        if (!m) throw new Error(`expected a number at ${this.i} in path data`);
        this.i = NUM_RE.lastIndex;
        return parseFloat(m[0]);
    }

    /** An arc flag: a single 0 or 1, which may run into the next number. */
    flag(): boolean {
        this.skip();
        const ch = this.d[this.i];
        if (ch !== "0" && ch !== "1") throw new Error(`expected an arc flag at ${this.i} in path data`);
        this.i++;
        return ch === "1";
    }
}

/** Parse path data to subpaths. Malformed data throws: a partial cut is worse than none. */
function parsePath(d: string): LocalSubpath[] {
    const r = new PathReader(d);
    const out: LocalSubpath[] = [];
    let cur: LocalSubpath = { curves: [], closed: false, zLine: false };
    let at: Pt = { x: 0, y: 0 };
    let start: Pt = at;
    let cubicCtrl: Pt | null = null; // last C/S second control point
    let quadCtrl: Pt | null = null;  // last Q/T control point
    let cmd = "";

    const flush = (): void => {
        if (cur.curves.length > 0) out.push(cur);
        cur = { curves: [], closed: false, zLine: false };
    };

    while (!r.done()) {
        const next = r.command();
        if (next !== null) cmd = next;
        else if (cmd === "" || cmd.toUpperCase() === "Z") throw new Error("path data must start with a command");
        else if (cmd === "M") cmd = "L";
        else if (cmd === "m") cmd = "l";

        const rel = cmd === cmd.toLowerCase();
        const C = cmd.toUpperCase();
        const pt = (): Pt => {
            const x = r.number(), y = r.number();
            return rel ? { x: at.x + x, y: at.y + y } : { x, y };
        };
        let nextCubic: Pt | null = null;
        let nextQuad: Pt | null = null;

        switch (C) {
            case "M":
                flush();
                at = start = pt();
                break;
            case "L": {
                const p = pt();
                cur.curves.push(lineToCubic(at, p));
                at = p;
                break;
            }
            case "H": {
                const x = r.number();
                const p = { x: rel ? at.x + x : x, y: at.y };
                cur.curves.push(lineToCubic(at, p));
                at = p;
                break;
            }
            case "V": {
                const y = r.number();
                const p = { x: at.x, y: rel ? at.y + y : y };
                cur.curves.push(lineToCubic(at, p));
                at = p;
                break;
            }
            case "C": {
                const p1 = pt(), p2 = pt(), p3 = pt();
                cur.curves.push(cubic(at, p1, p2, p3));
                at = p3;
                nextCubic = p2;
                break;
            }
            case "S": {
                const p1 = cubicCtrl ? { x: 2 * at.x - cubicCtrl.x, y: 2 * at.y - cubicCtrl.y } : at;
                const p2 = pt(), p3 = pt();
                cur.curves.push(cubic(at, p1, p2, p3));
                at = p3;
                nextCubic = p2;
                break;
            }
            case "Q": {
                const q = pt(), p = pt();
                cur.curves.push(quadToCubic(at, q, p));
                at = p;
                nextQuad = q;
                break;
            }
            case "T": {
                const q: Pt = quadCtrl ? { x: 2 * at.x - quadCtrl.x, y: 2 * at.y - quadCtrl.y } : at;
                const p = pt();
                cur.curves.push(quadToCubic(at, q, p));
                at = p;
                nextQuad = q;
                break;
            }
            case "A": {
                const rx = r.number(), ry = r.number(), phi = r.number();
                const large = r.flag(), sweep = r.flag();
                const p = pt();
                cur.curves.push(...arcToCubics(at, rx, ry, phi, large, sweep, p));
                at = p;
                break;
            }
            case "Z": {
                if (at.x !== start.x || at.y !== start.y) {
                    cur.curves.push(lineToCubic(at, start));
                    cur.zLine = true;
                }
                cur.closed = true;
                flush();
                at = start;
                break;
            }
        }
        cubicCtrl = nextCubic;
        quadCtrl = nextQuad;
    }
    flush();
    return out;
}

// ── shapes ────────────────────────────────────────────────────────────────────

const NO_PAINT = ["none", "transparent"];
const SHAPES = ["path", "circle", "ellipse", "rect", "line", "polygon", "polyline"];

function num(el: Element, attr: string): number {
    const v = el.getAttribute(attr);
    return v !== null ? parseFloat(v) : 0;
}

/** A property from `style=` first, then the presentation attribute; lowercased. */
function styleProp(el: Element, prop: string): string | null {
    for (const decl of (el.getAttribute("style") ?? "").split(";")) {
        const idx = decl.indexOf(":");
        if (idx >= 0 && decl.slice(0, idx).trim() === prop) return decl.slice(idx + 1).trim().toLowerCase();
    }
    const v = el.getAttribute(prop);
    return v ? v.trim().toLowerCase() : null;
}

function isPaintable(el: Element): boolean {
    const fill = styleProp(el, "fill");
    const stroke = styleProp(el, "stroke");
    if (fill === null || !NO_PAINT.includes(fill)) return true;
    return stroke !== null && !NO_PAINT.includes(stroke);
}

function ellipse(cx: number, cy: number, rx: number, ry: number): CubicBezier[] {
    const kx = rx * KAPPA, ky = ry * KAPPA;
    return [
        cubic({ x: cx + rx, y: cy }, { x: cx + rx, y: cy + ky }, { x: cx + kx, y: cy + ry }, { x: cx, y: cy + ry }),
        cubic({ x: cx, y: cy + ry }, { x: cx - kx, y: cy + ry }, { x: cx - rx, y: cy + ky }, { x: cx - rx, y: cy }),
        cubic({ x: cx - rx, y: cy }, { x: cx - rx, y: cy - ky }, { x: cx - kx, y: cy - ry }, { x: cx, y: cy - ry }),
        cubic({ x: cx, y: cy - ry }, { x: cx + kx, y: cy - ry }, { x: cx + rx, y: cy - ky }, { x: cx + rx, y: cy }),
    ];
}

function polyline(pts: Pt[], closed: boolean): CubicBezier[] {
    const curves = pts.slice(0, -1).map((p, i) => lineToCubic(p, pts[i + 1]!));
    if (closed) curves.push(lineToCubic(pts[pts.length - 1]!, pts[0]!));
    return curves;
}

/** Subpaths of one drawable element, in its own coordinates. */
function shapeSubpaths(el: Element): LocalSubpath[] {
    if (!isPaintable(el)) return [];
    const one = (curves: CubicBezier[], closed: boolean): LocalSubpath[] => [{ curves, closed, zLine: false }];

    switch (el.localName) {
        case "path":
            return parsePath(el.getAttribute("d") ?? "");
        case "circle": {
            const r = num(el, "r");
            return r > 0 ? one(ellipse(num(el, "cx"), num(el, "cy"), r, r), true) : [];
        }
        case "ellipse": {
            const rx = num(el, "rx"), ry = num(el, "ry");
            return rx > 0 && ry > 0 ? one(ellipse(num(el, "cx"), num(el, "cy"), rx, ry), true) : [];
        }
        case "rect": {
            const x = num(el, "x"), y = num(el, "y"), w = num(el, "width"), h = num(el, "height");
            if (!(w > 0 && h > 0)) return [];
            let rx = num(el, "rx"), ry = num(el, "ry") || rx;
            rx = Math.min(rx || ry, w / 2);
            ry = Math.min(ry, h / 2);
            if (rx <= 0 || ry <= 0) {
                return one(polyline([{ x, y }, { x: x + w, y }, { x: x + w, y: y + h }, { x, y: y + h }], true), true);
            }
            return parsePath(
                `M ${x + rx},${y} H ${x + w - rx} A ${rx} ${ry} 0 0 1 ${x + w},${y + ry} ` +
                `V ${y + h - ry} A ${rx} ${ry} 0 0 1 ${x + w - rx},${y + h} ` +
                `H ${x + rx} A ${rx} ${ry} 0 0 1 ${x},${y + h - ry} ` +
                `V ${y + ry} A ${rx} ${ry} 0 0 1 ${x + rx},${y} Z`,
            );
        }
        case "line": {
            const p0 = { x: num(el, "x1"), y: num(el, "y1") }, p1 = { x: num(el, "x2"), y: num(el, "y2") };
            return one([lineToCubic(p0, p1)], false);
        }
        case "polygon":
        case "polyline": {
            const c = (el.getAttribute("points") ?? "").trim().split(/[\s,]+/).filter((v) => v).map(parseFloat);
            const pts: Pt[] = [];
            for (let i = 0; i + 1 < c.length; i += 2) pts.push({ x: c[i]!, y: c[i + 1]! });
            if (pts.length < 2) return [];
            const closed = el.localName === "polygon";
            return one(polyline(pts, closed), closed);
        }
    }
    return [];
}

// ── transforms ────────────────────────────────────────────────────────────────

const TRANSFORM_RE = /(matrix|translate|scale|rotate|skewX|skewY)\s*\(([^)]*)\)/g;

/** A `transform` attribute as one matrix; the list composes left to right. */
export function parseTransform(attr: string | null): Affine {
    let m = IDENTITY;
    if (!attr) return m;
    for (const [, name, body] of attr.matchAll(TRANSFORM_RE)) {
        const a = body!.trim().split(/[\s,]+/).filter((v) => v).map(parseFloat);
        const rad = ((a[0] ?? 0) * Math.PI) / 180;
        let t: Affine;
        switch (name) {
            case "matrix":
                if (a.length !== 6) throw new Error(`matrix() needs 6 numbers: '${attr}'`);
                t = { a: a[0]!, b: a[1]!, c: a[2]!, d: a[3]!, e: a[4]!, f: a[5]! };
                break;
            case "translate":
                t = { ...IDENTITY, e: a[0] ?? 0, f: a[1] ?? 0 };
                break;
            case "scale":
                t = { ...IDENTITY, a: a[0] ?? 1, d: a[1] ?? a[0] ?? 1 };
                break;
            case "rotate": {
                const cos = Math.cos(rad), sin = Math.sin(rad);
                const cx = a[1] ?? 0, cy = a[2] ?? 0;
                t = { a: cos, b: sin, c: -sin, d: cos, e: cx - cos * cx + sin * cy, f: cy - sin * cx - cos * cy };
                break;
            }
            case "skewX":
                t = { ...IDENTITY, c: Math.tan(rad) };
                break;
            default: // skewY
                t = { ...IDENTITY, b: Math.tan(rad) };
        }
        m = compose(m, t);
    }
    return m;
}

// ── viewport ──────────────────────────────────────────────────────────────────

const UNIT_TO_MM: Readonly<Record<string, number>> = {
    mm: 1, cm: 10, in: 25.4, pt: 25.4 / 72, pc: 25.4 / 6, px: 25.4 / 96, "": 25.4 / 96,
};

function toMm(value: string): number {
    const m = value.match(/^\s*([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*(mm|cm|in|pt|pc|px)?\s*$/);
    if (!m) throw new Error(`cannot parse dimension '${value}'`);
    return parseFloat(m[1]!) * UNIT_TO_MM[(m[2] ?? "").toLowerCase()]!;
}

/** viewBox and width/height; without a size, viewBox units are mm. */
function readViewport(root: Element): Viewport {
    const vb = (root.getAttribute("viewBox") ?? "").trim();
    let vbMinX = 0, vbMinY = 0, vbW: number, vbH: number;
    if (vb) {
        const p = vb.split(/\s+/).map(parseFloat);
        if (p.length !== 4) throw new Error(`invalid viewBox '${vb}'`);
        [vbMinX, vbMinY, vbW, vbH] = p as [number, number, number, number];
    } else {
        vbW = parseFloat(root.getAttribute("width") ?? "100");
        vbH = parseFloat(root.getAttribute("height") ?? "100");
    }
    const w = root.getAttribute("width"), h = root.getAttribute("height");
    const sized = w && h;
    return { vbMinX, vbMinY, vbW, vbH, widthMm: sized ? toMm(w) : vbW, heightMm: sized ? toMm(h) : vbH };
}

/** viewBox to mm with the Y flip (SVG +Y down, machine +Y up). */
function viewportMatrix(vp: Viewport): Affine {
    const sx = vp.widthMm / vp.vbW, sy = vp.heightMm / vp.vbH;
    return { a: sx, b: 0, c: 0, d: -sy, e: -vp.vbMinX * sx, f: vp.heightMm + vp.vbMinY * sy };
}

// ── document walk ─────────────────────────────────────────────────────────────

const INKSCAPE_NS = "http://www.inkscape.org/namespaces/inkscape";
const CONDITIONS = ["requiredFeatures", "requiredExtensions", "systemLanguage"];

/** The children a container renders: for <switch>, the first without conditions. */
function rendered(el: Element): Element[] {
    const kids = Array.from(el.children);
    if (el.localName !== "switch") return kids;
    const first = kids.find((k) => !CONDITIONS.some((a) => k.hasAttribute(a)));
    return first ? [first] : [];
}

/** Parse SVG text to layers of subpaths in mm, Y up. */
export function loadSvgPaths(svgText: string, options: LoadOptions = {}): LoadedSvg {
    const zTol = options.zTol ?? DEFAULT_Z_TOL;
    const doc = new DOMParser().parseFromString(svgText, "image/svg+xml");
    const root = doc.documentElement;
    if (!root || root.localName !== "svg") throw new Error("not a valid SVG document");

    const viewport = readViewport(root);
    const layers = new Map<string, Subpath[]>();

    const emit = (layer: string, m: Affine, local: LocalSubpath): void => {
        const curves = local.curves.map((c) => applyCubic(m, c));
        if (local.zLine) {
            const z = curves[curves.length - 1]!;
            if (length(sub(z.p3, z.p0)) < zTol) curves.pop();
        }
        if (curves.length === 0) return;
        const list = layers.get(layer);
        const sp = { curves, closed: local.closed };
        if (list) list.push(sp);
        else layers.set(layer, [sp]);
    };

    const walk = (el: Element, parent: Affine, layer: string): void => {
        const tag = el.localName;
        let m = parent;
        if (tag === "svg" && el !== root) m = compose(m, { ...IDENTITY, e: num(el, "x"), f: num(el, "y") });
        m = compose(m, parseTransform(el.getAttribute("transform")));

        if (tag === "svg" || tag === "g" || tag === "a" || tag === "switch") {
            let childLayer = layer;
            if (tag === "g") {
                const label = el.getAttributeNS(INKSCAPE_NS, "label") || el.getAttribute("id");
                if (label) childLayer = layer ? `${layer}/${label}` : label;
            }
            for (const child of rendered(el)) walk(child, m, childLayer);
            return;
        }
        if (SHAPES.includes(tag)) {
            for (const local of shapeSubpaths(el)) emit(layer, m, local);
        }
        // Anything else (defs, clipPath, mask, symbol, marker, pattern, text…) draws nothing.
    };

    walk(root, viewportMatrix(viewport), "");
    return { layers, viewport };
}
