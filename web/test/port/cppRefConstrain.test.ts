/**
 * cppRefConstrain.test.ts — reference vectors for the C++ constrain port.
 *
 * Same contract as the other two generators: bit-equality, inputs carried
 * alongside outputs, doubles as raw IEEE-754 hex.
 *
 * Constrain's input is a Sample[], which flatten already produces on both
 * sides — so rather than invent sample streams, most cases here are real
 * flatten output fed straight in. That keeps the two stages' fixtures in
 * agreement and means a constrain case is reachable geometry, not a shape that
 * only a test can build. A handful of hand-built streams cover the branches
 * real geometry does not reach.
 *
 *     case <name>
 *     opts <feedMax> <aMax> <junctionDeviation> <aRateDegS> <aAccelDegS2>
 *          <cornerStopAngleDeg|-> <vMin> <forcedStops:comma|->
 *     n <sampleCount>
 *     i <x> <y> <theta> <kappa> <ds> <flags:int>        (input samples)
 *     o <vCeiling>                                       (expected, in order)
 *     end
 *
 * Also emits junctionCap cases directly, in the same <fn> form cppRef.test.ts
 * uses, because it is exported for exactly that reason and its two epsilon
 * branches are hard to reach through constrain.
 *
 *     GEN_CPP_REF=1 npx vitest run test/port/cppRefConstrain
 */

import { it } from "vitest";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { constrain, junctionCap, type ConstrainOptions } from "../../src/toolpath/constrain.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import { qualityConfig } from "../../src/machine/index.js";
import { KAPPA, type CubicBezier } from "../../src/toolpath/geometry.js";
import {
    CURVE_BOUNDARY,
    PATH_START,
    PATH_END,
    type Sample,
} from "../../src/toolpath/sample.js";
import { hex } from "./refFormat.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "../../../test/data/constrain_ref.txt");
const SVG = join(HERE, "../production/data");

it("generates the C++ constrain reference", (ctx) => {
    if (process.env.GEN_CPP_REF !== "1") {
        ctx.skip();
        return;
    }

    const q = qualityConfig();
    const lines: string[] = [];

    function emitCase(name: string, samples: Sample[], o: ConstrainOptions): void {
        const got = constrain(samples, o);
        lines.push(`case ${name}`);
        lines.push(
            "opts " +
            [o.feedMax, o.aMax, o.junctionDeviation,
             o.aRateDegS ?? 0, o.aAccelDegS2 ?? 0].map(hex).join(" ") + " " +
            (o.cornerStopAngleDeg === undefined ? "-" : hex(o.cornerStopAngleDeg)) + " " +
            hex(o.vMin ?? 0) + " " +
            (o.forcedStops && o.forcedStops.size > 0
                ? [...o.forcedStops].sort((a, b) => a - b).join(",")
                : "-"),
        );
        lines.push(`n ${samples.length}`);
        for (const s of samples) {
            lines.push(
                "i " + [s.x, s.y, s.theta, s.kappa, s.ds].map(hex).join(" ") + ` ${s.flags}`,
            );
        }
        for (const c of got) lines.push("o " + hex(c.vCeiling));
        lines.push("end");
    }

    // The shipped machine's numbers, so the default case is the one that bakes
    // the golden — not a rounder set chosen for readability.
    const base: ConstrainOptions = {
        feedMax: 60,
        aMax: 800,
        junctionDeviation: q.junctionDeviation,
        aRateDegS: 720,
        aAccelDegS2: 3600,
        cornerStopAngleDeg: 30,
        vMin: q.vMin,
    };

    // ── real flatten output ──────────────────────────────────────────────────
    const fo = {
        chordTol: q.chordTol, dsMax: q.dsMax, dthetaMax: q.dthetaMax,
        dtMax: q.dtMax, dtMin: q.dtMin, maxRefine: q.maxRefine,
    };
    const curves: Record<string, CubicBezier[]> = {
        line: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 }, p2: { x: 20, y: 0 }, p3: { x: 30, y: 0 } }],
        quarter_circle_r5: [{
            p0: { x: 5, y: 0 }, p1: { x: 5, y: 5 * KAPPA },
            p2: { x: 5 * KAPPA, y: 5 }, p3: { x: 0, y: 5 },
        }],
        cusp: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 }, p2: { x: -10, y: 0 }, p3: { x: 0, y: 0 } }],
        near_cusp: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0.01 }, p2: { x: -10, y: 0.01 }, p3: { x: 0, y: 0 } }],
        s_curve: [{ p0: { x: 0, y: 0 }, p1: { x: 3, y: 7 }, p2: { x: 9, y: -7 }, p3: { x: 12, y: 0 } }],
        // Two curves joining at a hard corner: the only way to reach the
        // junction-deviation branch, which needs CURVE_BOUNDARY set.
        corner_join: [
            { p0: { x: 0, y: 0 }, p1: { x: 3, y: 0 }, p2: { x: 7, y: 0 }, p3: { x: 10, y: 0 } },
            { p0: { x: 10, y: 0 }, p1: { x: 10, y: 3 }, p2: { x: 10, y: 7 }, p3: { x: 10, y: 10 } },
        ],
        // A shallow join: below cornerStopAngleDeg, so the junction cap applies
        // instead of the stop. With only the hard corner above, `cap = 0` would
        // shadow junctionCap entirely and the whole branch would go unpinned.
        shallow_join: [
            { p0: { x: 0, y: 0 }, p1: { x: 3, y: 0 }, p2: { x: 7, y: 0 }, p3: { x: 10, y: 0 } },
            { p0: { x: 10, y: 0 }, p1: { x: 13, y: 0.5 }, p2: { x: 17, y: 1.2 }, p3: { x: 20, y: 2 } },
        ],
    };

    for (const [name, sp] of Object.entries(curves)) {
        const samples = flatten([sp], fo);
        emitCase(name, samples, base);
        // Each optional switch OFF on its own, so a mutant that ignores the
        // gate rather than the term still has somewhere to show up.
        emitCase(`${name}__noA`, samples, { ...base, aRateDegS: 0, aAccelDegS2: 0 });
        emitCase(`${name}__noARate`, samples, { ...base, aRateDegS: 0 });
        emitCase(`${name}__noAAccel`, samples, { ...base, aAccelDegS2: 0 });
        emitCase(`${name}__noCornerStop`, samples, { ...base, cornerStopAngleDeg: undefined });
        emitCase(`${name}__noVMin`, samples, { ...base, vMin: 0 });
        // vMin high enough to zero most of a curve: pins the floor as a
        // threshold rather than just as "sometimes on".
        emitCase(`${name}__vMinHigh`, samples, { ...base, vMin: 40 });
        // cornerStopAngleDeg = 0 is the case the C++ presence flag exists for:
        // every sample after the first stops. A port collapsing undefined and 0
        // produces the opposite answer here, not a near one.
        emitCase(`${name}__cornerStop0`, samples, { ...base, cornerStopAngleDeg: 0 });
        if (samples.length > 3) {
            emitCase(`${name}__forced`, samples, {
                ...base,
                // Includes an out-of-range index, which must be ignored rather
                // than throw or shift the others.
                forcedStops: new Set([0, 2, samples.length - 1, samples.length + 50]),
            });
        }
    }

    // Real geometry, off the same fixtures the golden is baked from. This is
    // where the volume comes from, and the only place the cap chain is
    // exercised against sample streams nobody designed.
    for (const svg of ["test_circle.svg", "fish.svg"]) {
        const { subpaths } = loadSvgMmSubpaths(readFileSync(join(SVG, svg), "utf-8"));
        const repaired = subpaths.map(
            (sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired,
        );
        const samples = flatten(repaired, fo);
        const stem = svg.replace(".svg", "");
        emitCase(stem, samples, base);
        emitCase(`${stem}__noA`, samples, { ...base, aRateDegS: 0, aAccelDegS2: 0 });
        emitCase(`${stem}__noVMin`, samples, { ...base, vMin: 0 });
    }

    // ── hand-built streams for what geometry will not produce ────────────────

    // kappaPrime's guards. Real flatten output never lands on them: the span
    // guard needs two consecutive ~zero ds, and the endpoint returns need a
    // 3-sample stream to be visible on their own.
    const mk = (x: number, kappa: number, ds: number, flags = 0): Sample =>
        ({ x, y: 0, theta: 0, kappa, ds, flags });
    emitCase("kp_span_guard", [
        mk(0, 0.0, 1e-7, PATH_START), mk(1, 0.5, 1e-7), mk(2, 0.0, 0, PATH_END),
    ], base);
    emitCase("kp_endpoints", [
        mk(0, 0.9, 1, PATH_START), mk(1, 0.1, 1), mk(2, 0.9, 0, PATH_END),
    ], base);
    // A CURVE_BOUNDARY at i+1 must zero kappaPrime at i even though sample i
    // itself is unflagged — the half of the flag test that the i-only reading
    // of this code would miss.
    emitCase("kp_break_at_next", [
        mk(0, 0.0, 1, PATH_START), mk(1, 0.4, 1), mk(2, 9.0, 1, CURVE_BOUNDARY),
        mk(3, 0.4, 0, PATH_END),
    ], base);
    // Single sample, and empty: the i > 0 guard and the loop itself.
    emitCase("single", [mk(0, 0.5, 0, PATH_START | PATH_END)], base);
    emitCase("empty", [], base);
    // kappa exactly at and around the 1e-9 gate.
    emitCase("kappa_gate", [
        mk(0, 0, 1, PATH_START), mk(1, 1e-9, 1), mk(2, 1.0000001e-9, 1),
        mk(3, 1e-8, 0, PATH_END),
    ], base);

    // ── junctionCap directly ─────────────────────────────────────────────────
    // Its two epsilon branches (straight -> feedMax, reversal -> 0) sit at turn
    // angles that flatten's own tangent cap keeps samples away from, so through
    // constrain alone they are unreachable.
    const jcLines: string[] = [];
    const turns = [
        0, 1e-7, 1e-4, 0.001, 0.01, 0.1, 1, 5, 15, 29.9, 30, 45, 60, 90, 120,
        150, 179, 179.999, 180, -1, -45, -90, -179.999, -180, 181, 359, 360,
    ];
    for (const t of turns) {
        for (const [aLat, dev, fm] of [[800, 0.05, 60], [100, 0.01, 5], [800, 0, 60]]) {
            jcLines.push(["junctionCap", 4, ...[t, aLat!, dev!, fm!].map(hex),
                          1, hex(junctionCap(t, aLat!, dev!, fm!))].join(" "));
        }
    }
    // Math.cos in bulk. constrain reaches it only through junctionCap's narrow
    // [0, pi/2] argument range, and a handful of angles is exactly the sample
    // size that reported a false pass for atan2 (docs/planner_audit.md).
    let seed = 987654321 >>> 0;
    const rnd = (): number => {
        seed ^= seed << 13; seed >>>= 0;
        seed ^= seed >>> 17;
        seed ^= seed << 5; seed >>>= 0;
        return seed / 4294967296;
    };
    for (let i = 0; i < 3000; i++) {
        // The call shape junctionCap actually makes.
        const a = (Math.abs(rnd() * 360 - 180) * Math.PI / 180) / 2;
        jcLines.push(["jsCos", 1, hex(a), 1, hex(Math.cos(a))].join(" "));
        // And a wider sweep, so the reduction branches are covered if a later
        // stage calls cos on something that is not a half-angle.
        const w = (rnd() * 2 - 1) * Math.pow(10, rnd() * 5 - 2);
        jcLines.push(["jsCos", 1, hex(w), 1, hex(Math.cos(w))].join(" "));
    }
    for (const v of [0, -0, Math.PI / 4, -Math.PI / 4, Math.PI / 2, Math.PI,
                     -Math.PI, 3 * Math.PI / 4, 1e-30, 1e-300, 2 ** -27,
                     1e5, -1e5, 1000000, 1.5707963267948966]) {
        jcLines.push(["jsCos", 1, hex(v), 1, hex(Math.cos(v))].join(" "));
    }

    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(
        OUT,
        "# GENERATED by web/test/port/cppRefConstrain.test.ts — do not edit by hand.\n" +
        "# GEN_CPP_REF=1 npx vitest run test/port/cppRefConstrain\n" +
        "# fn lines: <fn> <nIn> <in..> <nOut> <out..>   (doubles as raw IEEE-754 hex)\n" +
        jcLines.join("\n") + "\n" + lines.join("\n") + "\n",
    );
    // eslint-disable-next-line no-console
    console.log(`wrote ${lines.filter((l) => l.startsWith("case")).length} constrain cases, ` +
                `${lines.filter((l) => l.startsWith("o ")).length} ceilings, ` +
                `${jcLines.length} fn cases`);
});
