/**
 * cppRefPlan.test.ts — reference vectors for the C++ plan port.
 *
 * Runs AFTER the contract tests in the new order (docs/port_workflow.md): the
 * contract tests establish that the stage does its job, this establishes that
 * it does it with the same bits as the TypeScript. Semantic defects are meant
 * to be gone by the time anything here fails, so a failure here should only
 * ever be numeric.
 *
 *     case <name>
 *     opts <xAccel> <yAccel> <aAccelDegS2> <aMax> <pathAccel>
 *     n <count>
 *     i <x> <y> <theta> <kappa> <ds> <flags:int> <vCeiling>
 *     o <v>
 *     end
 *
 * Plus segAccel directly, in the <fn> form the other generators use, because
 * its per-axis branches are reachable far more cheaply as a unit than through
 * geometry that happens to travel in the right direction.
 *
 *     GEN_CPP_REF=1 npx vitest run test/port/cppRefPlan
 */

import { it } from "vitest";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { plan, segAccel, type PlanOptions } from "../../src/toolpath/plan.js";
import { constrain, type ConstrainedSample } from "../../src/toolpath/constrain.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import { qualityConfig } from "../../src/config/config.js";
import { lineToCubic, type CubicBezier } from "../../src/toolpath/geometry.js";
import type { Sample } from "../../src/toolpath/sample.js";
import { hex } from "./refFormat.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "../../../test/test_motion/data/plan_ref.txt");
const SVG = join(HERE, "../production/data");

const X_ACCEL = 1000, Y_ACCEL = 1000, A_ACCEL = 2000, A_MAX = 1000, FEED = 80;

it("generates the C++ plan reference", (ctx) => {
    if (process.env.GEN_CPP_REF !== "1") {
        ctx.skip();
        return;
    }

    const q = qualityConfig();
    const lines: string[] = [];
    const fnLines: string[] = [];

    const base: PlanOptions = {
        xAccel: X_ACCEL, yAccel: Y_ACCEL, aAccelDegS2: A_ACCEL, aMax: A_MAX,
    };

    function emitCase(name: string, input: ConstrainedSample[], o: PlanOptions): void {
        const got = plan(input, o);
        lines.push(`case ${name}`);
        lines.push("opts " + [o.xAccel, o.yAccel, o.aAccelDegS2, o.aMax,
                              o.pathAccel ?? 0].map(hex).join(" "));
        lines.push(`n ${input.length}`);
        for (const s of input) {
            lines.push("i " + [s.x, s.y, s.theta, s.kappa, s.ds].map(hex).join(" ") +
                       ` ${s.flags} ` + hex(s.vCeiling));
        }
        for (const p of got) lines.push("o " + hex(p.v));
        lines.push("end");
    }

    const constrained = (sp: readonly (readonly CubicBezier[])[],
                         aRate = 0, cornerStop?: number): ConstrainedSample[] =>
        constrain(flatten(sp, q), {
            feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation,
            aRateDegS: aRate, aAccelDegS2: A_ACCEL,
            cornerStopAngleDeg: cornerStop, vMin: q.vMin,
        });

    const line = (a: [number, number], b: [number, number]): CubicBezier =>
        lineToCubic({ x: a[0], y: a[1] }, { x: b[0], y: b[1] });

    const geo: Record<string, CubicBezier[]> = {
        straight_line: [line([0, 0], [100, 0])],
        short_line: [line([0, 0], [4, 0])],
        diagonal: [line([0, 0], [50, 50])],
        quarter_circle_r5: [{ p0: { x: 5, y: 0 }, p1: { x: 5, y: 2.761 },
                              p2: { x: 2.761, y: 5 }, p3: { x: 0, y: 5 } }],
        s_curve: [{ p0: { x: 0, y: 0 }, p1: { x: 20, y: 40 },
                    p2: { x: 40, y: 40 }, p3: { x: 60, y: 0 } },
                  { p0: { x: 60, y: 0 }, p1: { x: 80, y: -40 },
                    p2: { x: 100, y: -40 }, p3: { x: 120, y: 0 } }],
        cusp: [{ p0: { x: 0, y: 0 }, p1: { x: 10, y: 0 },
                 p2: { x: 0, y: 5 }, p3: { x: 10, y: -5 } }],
        near_cusp: [{ p0: { x: 0, y: 0 }, p1: { x: 40, y: 0 },
                      p2: { x: 41, y: 1 }, p3: { x: 1, y: 1 } }],
        elbow: [line([0, 0], [20, 0]), line([20, 0], [20, 20])],
    };

    for (const [name, sp] of Object.entries(geo)) {
        const c = constrained([sp]);
        emitCase(name, c, base);
        // Each optional/limit varied on its own, so a mutant that ignores one
        // term still has somewhere to show up.
        emitCase(`${name}__pathAccel100`, c, { ...base, pathAccel: 100 });
        emitCase(`${name}__noX`, c, { ...base, xAccel: 0 });
        emitCase(`${name}__noY`, c, { ...base, yAccel: 0 });
        emitCase(`${name}__noA`, c, { ...base, aAccelDegS2: 0 });
        // aMax 0 disables the P1 headroom block entirely — the branch that
        // couples constrain's ceilings back into plan's accel budget.
        emitCase(`${name}__noAMax`, c, { ...base, aMax: 0 });
        emitCase(`${name}__nonSquare`, c, { ...base, xAccel: 500, yAccel: 2000 });
        // A tangential tool: constrain's A caps bite, so plan sees a very
        // different ceiling profile going in.
        emitCase(`${name}__tangential`, constrained([sp], 720), base);
        // A corner stop mid-subpath: vCeiling 0 in the interior.
        emitCase(`${name}__cornerStop`, constrained([sp], 0, 30), base);
    }

    // Multi-subpath, so the per-subpath independence of the sweeps is pinned.
    emitCase("multi", constrained([geo.straight_line!, geo.s_curve!, geo.quarter_circle_r5!]), base);

    // Real geometry off the golden's own fixtures.
    for (const svg of ["test_circle.svg", "fish.svg"]) {
        const { subpaths } = loadSvgMmSubpaths(readFileSync(join(SVG, svg), "utf-8"));
        const repaired = subpaths.map(
            (sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired,
        );
        const c = constrained(repaired);
        const stem = svg.replace(".svg", "");
        emitCase(stem, c, base);
        emitCase(`${stem}__pathAccel100`, c, { ...base, pathAccel: 100 });
    }

    // ── segAccel directly ────────────────────────────────────────────────────
    let seed = 24680 >>> 0;
    const rnd = (): number => {
        seed ^= seed << 13; seed >>>= 0;
        seed ^= seed >>> 17;
        seed ^= seed << 5; seed >>>= 0;
        return seed / 4294967296;
    };
    const mk = (x: number, y: number, kappa: number): Sample =>
        ({ x, y, theta: 0, kappa, ds: 0, flags: 0 });
    const optSets: PlanOptions[] = [
        base,
        { ...base, pathAccel: 250 },
        { ...base, xAccel: 0 },
        { ...base, yAccel: 0 },
        { ...base, aAccelDegS2: 0 },
        { ...base, xAccel: 500, yAccel: 2000 },
    ];
    for (let i = 0; i < 1500; i++) {
        const s0 = mk((rnd() * 2 - 1) * 50, (rnd() * 2 - 1) * 50,
                      Math.pow(10, rnd() * 12 - 11));
        const s1 = mk(s0.x + (rnd() * 2 - 1) * 5, s0.y + (rnd() * 2 - 1) * 5,
                      Math.pow(10, rnd() * 12 - 11));
        const o = optSets[i % optSets.length]!;
        fnLines.push(["segAccel", 8,
                      ...[s0.x, s0.y, s0.kappa, s1.x, s1.y, s1.kappa].map(hex),
                      ...[o.xAccel, o.yAccel].map(hex),
                      3, ...[o.aAccelDegS2, o.aMax, o.pathAccel ?? 0].map(hex),
                      1, hex(segAccel(s0, s1, o))].join(" "));
    }
    // Exact-boundary cases the random sweep will not hit: pure axes, the
    // degenerate segment, and the two epsilon gates on ux/uy.
    for (const [x0, y0, x1, y1, k] of [
        [0, 0, 10, 0, 0], [0, 0, 0, 10, 0], [0, 0, 10, 10, 0],
        [3, 3, 3, 3, 0],                       // degenerate -> aMax
        [0, 0, 1e-13, 0, 0],                   // below the d gate
        [0, 0, 1, 1e-10, 0],                   // uy below its gate
        [0, 0, 1, 0, 1e-9], [0, 0, 1, 0, 1e-10], [0, 0, 1, 0, 1e-8],
    ]) {
        const s0 = mk(x0!, y0!, k!);
        const s1 = mk(x1!, y1!, k!);
        for (const o of optSets) {
            fnLines.push(["segAccel", 8,
                          ...[s0.x, s0.y, s0.kappa, s1.x, s1.y, s1.kappa].map(hex),
                          ...[o.xAccel, o.yAccel].map(hex),
                          3, ...[o.aAccelDegS2, o.aMax, o.pathAccel ?? 0].map(hex),
                          1, hex(segAccel(s0, s1, o))].join(" "));
        }
    }

    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(
        OUT,
        "# GENERATED by web/test/port/cppRefPlan.test.ts — do not edit by hand.\n" +
        "# GEN_CPP_REF=1 npx vitest run test/port/cppRefPlan\n" +
        fnLines.join("\n") + "\n" + lines.join("\n") + "\n",
    );
    // eslint-disable-next-line no-console
    console.log(`wrote ${lines.filter((l) => l.startsWith("case")).length} plan cases, ` +
                `${lines.filter((l) => l.startsWith("o ")).length} speeds, ` +
                `${fnLines.length} segAccel cases`);
});
