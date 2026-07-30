/**
 * cppRef.test.ts — generate the C++ port's differential reference vectors.
 *
 * The port's pass criterion is BIT-EQUALITY with this TypeScript, not
 * closeness (docs/planner_audit.md, "Numeric porting rule"). That only works
 * if the two implementations can be fed identical inputs and compared without
 * a decimal round-trip in the way, so every number here crosses as its raw
 * IEEE-754 bit pattern in hex.
 *
 * Emitted format, one case per line:
 *
 *     <fn> <nIn> <in0> .. <inN-1> <nOut> <out0> .. <outM-1>
 *
 * The file carries INPUTS as well as outputs on purpose: the C++ side reads
 * the inputs and computes its own outputs, so the case list lives in exactly
 * one place and cannot drift between the languages.
 *
 * This test does not assert — it is a generator, and it is skipped unless
 * asked, the same shape as the golden's UPDATE_GOLDEN=1:
 *
 *     GEN_CPP_REF=1 npx vitest run test/port/cppRef
 *
 * Regenerate whenever a ported function's arithmetic intentionally changes,
 * and review the diff — a moved line here means moved bytes on the wire.
 */

import { it } from "vitest";
import { writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import {
    lineToCubic,
    quadToCubic,
    length,
    normalize,
    angleBetweenDeg,
    exitTangent,
    entryTangent,
    angleDelta,
    bezierPoint,
    bezierDeriv1,
    bezierDeriv2,
    curvature,
    KAPPA,
    type CubicBezier,
    type Pt,
} from "../../src/toolpath/geometry.js";

const OUT = join(
    dirname(fileURLToPath(import.meta.url)),
    "../../../test/test_motion/data/geometry_ref.txt",
);

/** A double as its exact 16-hex-digit IEEE-754 bit pattern. */
function hex(v: number): string {
    const dv = new DataView(new ArrayBuffer(8));
    dv.setFloat64(0, v, false);
    let s = "";
    for (let i = 0; i < 8; i++) s += dv.getUint8(i).toString(16).padStart(2, "0");
    return s;
}

const lines: string[] = [];
function emit(fn: string, ins: number[], outs: number[]): void {
    lines.push(
        [fn, ins.length, ...ins.map(hex), outs.length, ...outs.map(hex)].join(" "),
    );
}

const flat = (c: CubicBezier): number[] => [
    c.p0.x, c.p0.y, c.p1.x, c.p1.y, c.p2.x, c.p2.y, c.p3.x, c.p3.y,
];
const pt = (p: Pt): number[] => [p.x, p.y];

it("generates the C++ differential reference", (ctx) => {
    if (process.env.GEN_CPP_REF !== "1") {
        ctx.skip();
        return;
    }

    // Curves chosen to span the geometry the planner actually meets, including
    // the two the audit spent batches on: a true cusp (tangent reverses at a
    // single t) and a near-cusp (very high curvature, never quite reversing).
    const curves: [string, CubicBezier][] = [
        ["line", { p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 }, p2: { x: 20, y: 0 }, p3: { x: 30, y: 0 } }],
        ["quarter_circle_r5", {
            p0: { x: 5, y: 0 }, p1: { x: 5, y: 5 * KAPPA },
            p2: { x: 5 * KAPPA, y: 5 }, p3: { x: 0, y: 5 },
        }],
        ["cusp", { p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 }, p2: { x: -10, y: 0 }, p3: { x: 0, y: 0 } }],
        ["near_cusp", { p0: { x: 0, y: 0 }, p1: { x: 10, y: 0.01 }, p2: { x: -10, y: 0.01 }, p3: { x: 0, y: 0 } }],
        ["s_curve", { p0: { x: 0, y: 0 }, p1: { x: 3, y: 7 }, p2: { x: 9, y: -7 }, p3: { x: 12, y: 0 } }],
        ["asymmetric", { p0: { x: -3.7, y: 11.25 }, p1: { x: 0.125, y: -4.5 }, p2: { x: 8.875, y: 2.25 }, p3: { x: 13.5, y: -0.125 } }],
        // Degenerate: all control points coincident. speed == 0, so curvature
        // must take its early-return branch in both languages.
        ["degenerate", { p0: { x: 2, y: 2 }, p1: { x: 2, y: 2 }, p2: { x: 2, y: 2 }, p3: { x: 2, y: 2 } }],
    ];

    // t values including both endpoints, the midpoint, the cusp parameter
    // (0.5), and values with no short binary representation so the low bits of
    // the polynomial evaluation are actually exercised.
    const ts = [0, 1, 0.5, 0.25, 0.75, 0.1, 0.9, 1 / 3, 2 / 3, 0.123456789, 0.987654321, 1e-9, 1 - 1e-9];

    for (const [, c] of curves) {
        for (const t of ts) {
            emit("bezierPoint", [...flat(c), t], pt(bezierPoint(c, t)));
            emit("bezierDeriv1", [...flat(c), t], pt(bezierDeriv1(c, t)));
            emit("bezierDeriv2", [...flat(c), t], pt(bezierDeriv2(c, t)));
            emit("curvature", [...flat(c), t], [curvature(c, t)]);
        }
        emit("exitTangent", flat(c), pt(exitTangent(c)));
        emit("entryTangent", flat(c), pt(entryTangent(c)));
    }

    const vecs: Pt[] = [
        { x: 3.7, y: -11.3 }, { x: 0, y: 0 }, { x: 1e-13, y: 1e-13 },
        { x: 1e-8, y: 2.5e-9 }, { x: -4.4, y: 4.4 }, { x: 12345.678, y: 0.0009 },
        { x: 1, y: 0 }, { x: 0, y: -1 }, { x: 0.1, y: 0.2 },
    ];
    for (const v of vecs) {
        emit("length", pt(v), [length(v)]);
        emit("normalize", pt(v), pt(normalize(v)));
        for (const u of vecs) emit("angleBetweenDeg", [...pt(u), ...pt(v)], [angleBetweenDeg(u, v)]);
        emit("lineToCubic", [0.5, -0.25, ...pt(v)], flat(lineToCubic({ x: 0.5, y: -0.25 }, v)));
        emit("quadToCubic", [0.5, -0.25, ...pt(v), 3, 4], flat(quadToCubic({ x: 0.5, y: -0.25 }, v, { x: 3, y: 4 })));
    }

    // angleDelta: the wrap loops, including inputs needing several turns and
    // exact multiples of 180 where the boundary condition (> vs >=) shows.
    const angles = [0, 1, -1, 90, -90, 179.9, 180, -180, 180.1, 359, 361, 720, -720, 1080.5, -1e6, 0.1];
    for (const a of angles) for (const b of angles) emit("angleDelta", [a, b], [angleDelta(a, b)]);

    // jsRound: Math.round has no TypeScript wrapper in the port's scope — it is
    // called inline — so this pins the primitive itself. The cases are the ones
    // that separate it from std::round and from floor(x + 0.5).
    const rounds = [
        0.5, -0.5, 1.5, -1.5, 2.5, -2.5, 0.49999999999999994, -0.49999999999999994,
        0, -0.2, 0.2, 4503599627370497.0, -4503599627370497.0, 1e21, -1e21,
        160000.5, -160000.5, 2.0000000000000004, -1e-300,
    ];
    for (const v of rounds) emit("jsRound", [v], [Math.round(v)]);

    // The three transcendentals the port had to OWN rather than take from the
    // platform libm (docs/planner_audit.md). Pinned directly and in bulk, not
    // just through their callers: mingw's libm disagrees with V8 on 17.6% of
    // atan2 inputs and 7.7% of acos inputs, so a handful of hand-picked cases
    // is exactly the sample size that reports a false pass. A deterministic
    // xorshift keeps the set reproducible; the magnitude sweep spans the range
    // Bezier derivatives actually reach.
    let seed = 12345 >>> 0;
    const rnd = (): number => {
        seed ^= seed << 13; seed >>>= 0;
        seed ^= seed >>> 17;
        seed ^= seed << 5; seed >>>= 0;
        return seed / 4294967296;
    };
    for (let i = 0; i < 4000; i++) {
        const mag = Math.pow(10, rnd() * 8 - 4);
        const x = (rnd() * 2 - 1) * mag;
        const y = (rnd() * 2 - 1) * mag;
        emit("jsAtan2", [y, x], [Math.atan2(y, x)]);
        emit("jsHypot", [x, y], [Math.hypot(x, y)]);
        const c = rnd() * 2 - 1; // bind once — two rnd() calls emit an input
        emit("jsAcos", [c], [Math.acos(c)]); // that is not the one measured

    }
    // Exact-boundary cases the random sweep will never hit.
    for (const [y, x] of [[0, 1], [-0, 1], [0, -1], [-0, -1], [1, 0], [-1, 0],
                          [1, 1], [-1, -1], [Infinity, 1], [1, Infinity],
                          [Infinity, Infinity], [-Infinity, -Infinity]]) {
        emit("jsAtan2", [y!, x!], [Math.atan2(y!, x!)]);
    }
    for (const v of [-1, -0.5, 0, 0.5, 1, 1e-300, -1e-300, 0.9999999999999999]) {
        emit("jsAcos", [v], [Math.acos(v)]);
    }
    for (const [a, b] of [[0, 0], [-0, 0], [1e-320, 1e-320], [1e308, 1e308], [3, 4]]) {
        emit("jsHypot", [a!, b!], [Math.hypot(a!, b!)]);
    }

    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(
        OUT,
        "# GENERATED by web/test/port/cppRef.test.ts — do not edit by hand.\n" +
        "# GEN_CPP_REF=1 npx vitest run test/port/cppRef\n" +
        "# <fn> <nIn> <in..> <nOut> <out..>   (doubles as raw IEEE-754 hex)\n" +
        lines.join("\n") + "\n",
    );
    // eslint-disable-next-line no-console
    console.log(`wrote ${lines.length} reference cases to ${OUT}`);
});
