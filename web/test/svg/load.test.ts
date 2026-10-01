/**
 * Tests for load.ts — SVG to subpaths in mm. Inputs are inline; with
 * `viewBox="0 0 100 100"` and no size, a point (x, y) lands at (x, 100 - y).
 */

import { describe, it, expect } from "vitest";
import { bezierPoint, type CubicBezier, type Pt } from "../../src/toolpath/geometry.js";
import { loadSvgPaths, type LoadOptions, type Subpath } from "../../src/svg/load.js";

function doc(body: string, attrs = 'viewBox="0 0 100 100"'): string {
    return `<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" ${attrs}>${body}</svg>`;
}

/** All subpaths of a document, layers flattened, back in SVG coordinates (Y down). */
function load(body: string, options?: LoadOptions): Subpath[] {
    const flip = (p: Pt): Pt => ({ x: p.x, y: 100 - p.y });
    return [...loadSvgPaths(doc(body), options).layers.values()].flat().map((s) => ({
        closed: s.closed,
        curves: s.curves.map((c) => ({ p0: flip(c.p0), p1: flip(c.p1), p2: flip(c.p2), p3: flip(c.p3) })),
    }));
}

function path(d: string): CubicBezier[] {
    return load(`<path d="${d}"/>`).flatMap((s) => s.curves);
}

function close(a: Pt, b: Pt, digits = 9): void {
    expect(a.x).toBeCloseTo(b.x, digits);
    expect(a.y).toBeCloseTo(b.y, digits);
}

/** The on-curve points: p0 of the first curve, then each p3. */
function ends(curves: readonly CubicBezier[]): Pt[] {
    return curves.length ? [curves[0]!.p0, ...curves.map((c) => c.p3)] : [];
}

describe("load: path grammar", () => {
    const cases: { d: string; ends: Pt[] }[] = [
        { d: "M10 10 L20 10 l0 10", ends: [{ x: 10, y: 10 }, { x: 20, y: 10 }, { x: 20, y: 20 }] },
        { d: "M10 10 H30 h-5 V20 v5", ends: [{ x: 10, y: 10 }, { x: 30, y: 10 }, { x: 25, y: 10 }, { x: 25, y: 20 }, { x: 25, y: 25 }] },
        { d: "M0 0 C1 1 2 1 3 0 c1 -1 2 -1 3 0", ends: [{ x: 0, y: 0 }, { x: 3, y: 0 }, { x: 6, y: 0 }] },
        { d: "M0 0 Q5 5 10 0 q5 -5 10 0", ends: [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 20, y: 0 }] },
        { d: "M0 0 A5 5 0 0 1 10 0 a5 5 0 0 1 10 0", ends: [{ x: 0, y: 0 }, { x: 5, y: -5 }, { x: 10, y: 0 }, { x: 15, y: -5 }, { x: 20, y: 0 }] },
        // implicit repeats: M then L pairs, m then l pairs, C repeated
        { d: "M0 0 10 0 10 10", ends: [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }] },
        { d: "m5 5 10 0 0 10", ends: [{ x: 5, y: 5 }, { x: 15, y: 5 }, { x: 15, y: 15 }] },
        { d: "M0 0 C0 1 1 1 1 0 1 -1 2 -1 2 0", ends: [{ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 2, y: 0 }] },
        // packed numbers and packed arc flags
        { d: "M1.5.5L-2-3", ends: [{ x: 1.5, y: 0.5 }, { x: -2, y: -3 }] },
        { d: "M0,0a5,5,0,0110,0", ends: [{ x: 0, y: 0 }, { x: 5, y: -5 }, { x: 10, y: 0 }] },
        { d: "M0 0 l10 0 m0 10 l-10 0", ends: [{ x: 0, y: 0 }, { x: 10, y: 0 }, { x: 10, y: 10 }, { x: 0, y: 10 }] },
    ];
    it.each(cases)("$d", (k) => {
        // Arcs add their 90° joins.
        const got = load(`<path d="${k.d}"/>`).flatMap((s) => ends(s.curves));
        expect(got).toHaveLength(k.ends.length);
        got.forEach((p, i) => close(p, k.ends[i]!, 6));
    });

    it("malformed data throws rather than cutting part of a path", () => {
        expect(() => path("M0 0 L10")).toThrow();
        expect(() => path("M0 0 X10 0")).toThrow();
        expect(() => path("10 0")).toThrow();
    });
});

describe("load: reflection", () => {
    it("S reflects the last C handle, else starts at the current point", () => {
        const [, s] = path("M0 0 C0 10 10 10 10 0 S20 -10 20 0");
        close(s!.p1, { x: 10, y: -10 });
        const [lone] = path("M0 0 S10 10 20 0");
        close(lone!.p1, { x: 0, y: 0 });
        const [, afterQ] = path("M0 0 Q5 5 10 0 S20 -10 20 0");
        close(afterQ!.p1, { x: 10, y: 0 });
    });

    it("T reflects the last Q control point, else its control is the current point", () => {
        // Q control (5,5) reflects to (15,-5); as a cubic p1 = p0 + 2/3·(q - p0).
        const [, t] = path("M0 0 Q5 5 10 0 T20 0");
        close(t!.p1, { x: 10 + (2 / 3) * 5, y: (2 / 3) * -5 });
        const [lone] = path("M0 0 T20 0");
        close(lone!.p1, { x: 0, y: 0 });
        close(lone!.p2, { x: 20 / 3, y: 0 });
    });
});

describe("load: Z", () => {
    it("adds a closing line, skips one under zTol, sets closed", () => {
        const [tri] = load(`<path d="M0 0 L10 0 L10 10 Z"/>`);
        expect(tri!.closed).toBe(true);
        expect(tri!.curves).toHaveLength(3);
        close(tri!.curves[2]!.p3, { x: 0, y: 0 });

        const [miss] = load(`<path d="M0 0 L10 0 L10 10 L0 1e-9 Z"/>`);
        expect(miss!.curves).toHaveLength(3);
        const [kept] = load(`<path d="M0 0 L10 0 L10 10 L0 1e-9 Z"/>`, { zTol: 1e-12 });
        expect(kept!.curves).toHaveLength(4);

        const [open] = load(`<path d="M0 0 L10 0"/>`);
        expect(open!.closed).toBe(false);
    });

    it("drawing after Z starts a new subpath at the start point", () => {
        const subs = load(`<path d="M5 5 L10 5 L10 10 Z L0 5"/>`);
        expect(subs.map((s) => s.closed)).toEqual([true, false]);
        close(subs[1]!.curves[0]!.p0, { x: 5, y: 5 });
    });
});

describe("load: shapes", () => {
    const cases = [
        { el: `<circle cx="50" cy="50" r="10"/>`, n: 4, closed: true, first: { x: 60, y: 50 } },
        { el: `<ellipse cx="50" cy="50" rx="20" ry="10"/>`, n: 4, closed: true, first: { x: 70, y: 50 } },
        { el: `<rect x="10" y="10" width="20" height="10"/>`, n: 4, closed: true, first: { x: 10, y: 10 } },
        { el: `<rect x="10" y="10" width="20" height="10" rx="2"/>`, n: 8, closed: true, first: { x: 12, y: 10 } },
        { el: `<line x1="1" y1="2" x2="3" y2="4"/>`, n: 1, closed: false, first: { x: 1, y: 2 } },
        { el: `<polygon points="0,0 10,0 10,10"/>`, n: 3, closed: true, first: { x: 0, y: 0 } },
        { el: `<polyline points="0,0 10,0 10,10"/>`, n: 2, closed: false, first: { x: 0, y: 0 } },
    ];
    it.each(cases)("$el", (k) => {
        const [s] = load(k.el);
        expect(s!.curves).toHaveLength(k.n);
        expect(s!.closed).toBe(k.closed);
        close(s!.curves[0]!.p0, k.first);
        if (k.closed) close(s!.curves[k.n - 1]!.p3, k.first);
    });

    it("zero sizes draw nothing", () => {
        expect(load(`<circle r="0"/><ellipse rx="5" ry="0"/><rect width="0" height="5"/><polygon points="1,1"/>`)).toEqual([]);
    });
});

describe("load: paint", () => {
    it("no fill and no stroke draws nothing", () => {
        expect(load(`<path d="M0 0 L1 1" fill="none"/>`)).toEqual([]);
        expect(load(`<path d="M0 0 L1 1" style="fill:none;stroke:none"/>`)).toEqual([]);
        expect(load(`<path d="M0 0 L1 1" style="fill:none" stroke="#000"/>`)).toHaveLength(1);
    });
});

describe("load: viewport", () => {
    const cases = [
        { attrs: 'viewBox="0 0 100 100" width="200mm" height="200mm"', pt: { x: 10, y: 10 }, mm: { x: 20, y: 180 } },
        { attrs: 'viewBox="0 0 100 100" width="10cm" height="10cm"', pt: { x: 10, y: 10 }, mm: { x: 10, y: 90 } },
        { attrs: 'viewBox="0 0 96 96" width="96px" height="96px"', pt: { x: 96, y: 0 }, mm: { x: 25.4, y: 25.4 } },
        { attrs: 'viewBox="50 30 100 100" width="100mm" height="100mm"', pt: { x: 50, y: 30 }, mm: { x: 0, y: 100 } },
        { attrs: 'viewBox="0 0 200 100" width="100mm" height="50mm"', pt: { x: 200, y: 100 }, mm: { x: 100, y: 0 } },
        { attrs: 'viewBox="0 0 100 60"', pt: { x: 10, y: 0 }, mm: { x: 10, y: 60 } },
    ];
    it.each(cases)("$attrs", (k) => {
        const { layers } = loadSvgPaths(doc(`<path d="M${k.pt.x} ${k.pt.y} l1 0"/>`, k.attrs));
        close([...layers.values()][0]![0]!.curves[0]!.p0, k.mm);
    });
});

describe("load: transforms", () => {
    // The point (1, 0) under each transform, in SVG coordinates.
    const cases = [
        { t: "translate(10 5)", p: { x: 11, y: 5 } },
        { t: "translate(10)", p: { x: 11, y: 0 } },
        { t: "scale(2 3)", p: { x: 2, y: 0 } },
        { t: "rotate(90)", p: { x: 0, y: 1 } },
        { t: "rotate(90 1 1)", p: { x: 2, y: 1 } },
        { t: "matrix(1 0 0 1 4 5)", p: { x: 5, y: 5 } },
        { t: "skewX(45)", p: { x: 1, y: 0 } },
        { t: "skewY(45)", p: { x: 1, y: 1 } },
        { t: "translate(10,0) rotate(90)", p: { x: 10, y: 1 } },
    ];
    it.each(cases)("$t", (k) => {
        const [s] = load(`<path transform="${k.t}" d="M1 0 l0 0.5"/>`);
        close(s!.curves[0]!.p0, k.p);
    });

    it("composes element, nested groups and the viewBox", () => {
        const [s] = load(`<g transform="translate(10 0)"><g transform="scale(2)"><path transform="translate(1 1)" d="M1 0 l1 0"/></g></g>`);
        close(s!.curves[0]!.p0, { x: 14, y: 2 });
    });
});

describe("load: walk", () => {
    it("skips non-rendered containers, enters a / switch / nested svg", () => {
        const subs = load(
            `<defs><path d="M0 0 L1 1"/></defs><clipPath><rect width="5" height="5"/></clipPath>` +
            `<mask><path d="M0 0 L1 1"/></mask><symbol><path d="M0 0 L1 1"/></symbol>` +
            `<a><path d="M1 0 L2 0"/></a>` +
            `<switch><path systemLanguage="xx" d="M9 9 L9 8"/><path d="M2 0 L3 0"/><path d="M8 8 L8 7"/></switch>` +
            `<svg x="10" y="20"><path d="M3 0 L4 0"/></svg>`,
        );
        expect(subs.map((s) => s.curves[0]!.p0)).toEqual([{ x: 1, y: 0 }, { x: 2, y: 0 }, { x: 13, y: 20 }]);
    });
});

describe("load: layers", () => {
    it("names nested groups with '/', passes unnamed groups through, keeps order", () => {
        const { layers } = loadSvgPaths(doc(
            `<path d="M0 0 L1 0"/>` +
            `<g inkscape:label="knife" id="ignored"><g><path d="M1 0 L2 0"/></g><g id="slot1"><path d="M2 0 L3 0"/></g></g>` +
            `<g id="crease"><path d="M3 0 L4 0"/></g><path d="M4 0 L5 0"/>`,
        ));
        expect([...layers.keys()]).toEqual(["", "knife", "knife/slot1", "crease"]);
        expect(layers.get("")!.map((s) => s.curves[0]!.p0.x)).toEqual([0, 4]);
    });
});

describe("load: arc under skew", () => {
    it("stays on the skewed ellipse", () => {
        // skewX(30) maps (x, y) to (x + y·tan30, y): undo it and check radius 1.
        const [s] = load(`<path transform="skewX(30)" d="M1 0 A1 1 0 1 1 -1 0 A1 1 0 1 1 1 0"/>`);
        const k = Math.tan(Math.PI / 6);
        for (const c of s!.curves) {
            for (let i = 0; i <= 8; i++) {
                const p = bezierPoint(c, i / 8);
                expect(Math.abs(Math.hypot(p.x - k * p.y, p.y) - 1)).toBeLessThan(3e-4);
            }
        }
    });
});
