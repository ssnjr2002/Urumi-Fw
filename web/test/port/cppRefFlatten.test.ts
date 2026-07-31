/**
 * cppRefFlatten.test.ts — reference vectors for the C++ flatten port.
 *
 * Same contract as cppRef.test.ts (bit-equality, inputs carried alongside
 * outputs) but a stage rather than a primitive, so the shapes are variable
 * length and the file is line-structured instead of one-case-per-line:
 *
 *     case <name>
 *     opts <chordTol> <dsMax> <dthetaMax> <dtMax> <dtMin> <maxRefine:int>
 *     sp <nCurves> <8*nCurves hex>        (one per subpath, in order)
 *     n <sampleCount>
 *     s <x> <y> <theta> <kappa> <ds> <flags:int>
 *     end
 *
 * The INPUT is repaired Bezier subpaths, which is the real stage boundary:
 * enforceC1 (stage 3) is host-side and out of the port's scope, so the C++
 * flatten is fed exactly what compileBlock feeds the TypeScript one.
 *
 *     GEN_CPP_REF=1 npx vitest run test/port/cppRefFlatten
 */

import { it } from "vitest";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { flatten } from "../../src/toolpath/flatten.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import { qualityConfig } from "../../src/config/config.js";
import { KAPPA, type CubicBezier } from "../../src/toolpath/geometry.js";
import { hex } from "./refFormat.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "../../../test/data/flatten_ref.txt");
const SVG = join(HERE, "../production/data");

interface Opts {
    chordTol: number; dsMax: number; dthetaMax: number;
    dtMax: number; dtMin: number; maxRefine: number;
}

const flat = (c: CubicBezier): number[] => [
    c.p0.x, c.p0.y, c.p1.x, c.p1.y, c.p2.x, c.p2.y, c.p3.x, c.p3.y,
];

it("generates the C++ flatten reference", (ctx) => {
    if (process.env.GEN_CPP_REF !== "1") {
        ctx.skip();
        return;
    }

    const q = qualityConfig();
    const lines: string[] = [];

    function emitCase(name: string, subpaths: CubicBezier[][], o: Opts): void {
        const samples = flatten(subpaths, o);
        lines.push(`case ${name}`);
        lines.push(
            "opts " +
            [o.chordTol, o.dsMax, o.dthetaMax, o.dtMax, o.dtMin].map(hex).join(" ") +
            ` ${o.maxRefine}`,
        );
        for (const sp of subpaths) {
            lines.push(`sp ${sp.length} ` + sp.flatMap(flat).map(hex).join(" "));
        }
        lines.push(`n ${samples.length}`);
        for (const s of samples) {
            lines.push(
                "s " + [s.x, s.y, s.theta, s.kappa, s.ds].map(hex).join(" ") + ` ${s.flags}`,
            );
        }
        lines.push("end");
    }

    const base: Opts = {
        chordTol: q.chordTol, dsMax: q.dsMax, dthetaMax: q.dthetaMax,
        dtMax: q.dtMax, dtMin: q.dtMin, maxRefine: q.maxRefine,
    };

    // Synthetic geometry, each chosen for a branch rather than for looking
    // like a real part: the cusp reaches the irreducible-turn break, the
    // near-cusp drives the refine loop hard without reaching it, the straight
    // line exercises the dsMax cap with the chord cap inactive, and the
    // degenerate curve makes both epsilon guards in dtAt skip.
    const curves: Record<string, CubicBezier[]> = {
        line: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 }, p2: { x: 20, y: 0 }, p3: { x: 30, y: 0 } }],
        quarter_circle_r5: [{
            p0: { x: 5, y: 0 }, p1: { x: 5, y: 5 * KAPPA },
            p2: { x: 5 * KAPPA, y: 5 }, p3: { x: 0, y: 5 },
        }],
        cusp: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 }, p2: { x: -10, y: 0 }, p3: { x: 0, y: 0 } }],
        near_cusp: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0.01 }, p2: { x: -10, y: 0.01 }, p3: { x: 0, y: 0 } }],
        s_curve: [{ p0: { x: 0, y: 0 }, p1: { x: 3, y: 7 }, p2: { x: 9, y: -7 }, p3: { x: 12, y: 0 } }],
        degenerate: [{ p0: { x: 2, y: 2 }, p1: { x: 2, y: 2 }, p2: { x: 2, y: 2 }, p3: { x: 2, y: 2 } }],
        // dtAt's two epsilon guards are branch boundaries that ordinary
        // geometry never lands near, so without these two the guard constants
        // are unpinned — mutation confirmed 1e-12 -> 1e-11 and 1e-9 -> 1e-8
        // both survived the rest of this file.
        //
        // |B'| == 3e-12 everywhere: inside the speed guard's dead band, so the
        // spacing and tangent caps must both be skipped.
        tiny_speed: [{
            p0: { x: 0, y: 0 }, p1: { x: 1e-12, y: 0 },
            p2: { x: 2e-12, y: 0 }, p3: { x: 3e-12, y: 0 },
        }],
        // kappa sweeps 1.67e-9 .. 1.33e-8, straddling the curvature guard, so
        // the tangent cap applies at some samples on this curve and not others.
        tiny_kappa: [{
            p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 },
            p2: { x: 20, y: 1e-6 }, p3: { x: 30, y: 0 },
        }],
        // Two curves in one subpath, meeting at a hard corner: pins
        // CURVE_BOUNDARY and the prevTheta carry-over across the join.
        corner_join: [
            { p0: { x: 0, y: 0 }, p1: { x: 3, y: 0 }, p2: { x: 7, y: 0 }, p3: { x: 10, y: 0 } },
            { p0: { x: 10, y: 0 }, p1: { x: 10, y: 3 }, p2: { x: 10, y: 7 }, p3: { x: 10, y: 10 } },
        ],
    };

    for (const [name, sp] of Object.entries(curves)) {
        emitCase(name, [sp], base);
        // maxRefine 0 is the pre-F7 path (caps as predictors, no measurement)
        // and 1 is the boundary where the loop can halve exactly once.
        emitCase(`${name}__refine0`, [sp], { ...base, maxRefine: 0 });
        emitCase(`${name}__refine1`, [sp], { ...base, maxRefine: 1 });
    }

    // A degenerate curve FOLLOWING a real one. The lone `degenerate` fixture
    // cannot pin tangentDeg's fallback argument: there prevTheta is itself 0,
    // so passing 0 instead of prevTheta is indistinguishable. Here the
    // preceding curve leaves prevTheta non-zero, which is the only arrangement
    // that tells the two apart (mutation: the fallback survived without this).
    emitCase("degenerate_after_curve",
             [[curves.s_curve![0]!, curves.degenerate![0]!]], base);

    // Multiple subpaths in one call: PATH_START / PATH_END bracketing and the
    // ds fill must stay within each subpath.
    emitCase("multi_subpath", [curves.line!, curves.s_curve!, curves.quarter_circle_r5!], base);
    // An empty subpath is skipped without emitting flags or breaking the ds fill.
    emitCase("empty_subpath", [curves.line!, [], curves.s_curve!], base);

    // Real geometry, straight off the fixtures the golden is baked from, fed at
    // the true stage boundary (post-enforceC1).
    for (const svg of ["test_circle.svg", "fish.svg"]) {
        const { subpaths } = loadSvgMmSubpaths(readFileSync(join(SVG, svg), "utf-8"));
        const repaired = subpaths.map(
            (sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired,
        );
        emitCase(svg.replace(".svg", ""), repaired, base);
    }

    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(
        OUT,
        "# GENERATED by web/test/port/cppRefFlatten.test.ts — do not edit by hand.\n" +
        "# GEN_CPP_REF=1 npx vitest run test/port/cppRefFlatten\n" +
        lines.join("\n") + "\n",
    );
    // eslint-disable-next-line no-console
    console.log(`wrote ${lines.filter((l) => l.startsWith("case")).length} flatten cases, ` +
                `${lines.filter((l) => l.startsWith("s ")).length} samples`);
});
