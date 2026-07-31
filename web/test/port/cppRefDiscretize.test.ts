/**
 * cppRefDiscretize.test.ts — reference vectors for the C++ discretize port.
 *
 * Runs AFTER the contract tests (docs/port_workflow.md): those establish that
 * the stage does its job, this establishes that it does it with the same bits.
 * A failure here should only ever be numeric.
 *
 * Three record kinds, because stage 7 is really three pieces of code and the
 * cheapest place to pin two of them is directly:
 *
 *   interval()   as `fn` lines — its per-axis rate floor, the pure-rotation
 *                branch and the major==0 branch are all reachable in one call,
 *                and finding them through geometry that happens to produce the
 *                right step deltas would be luck rather than coverage.
 *
 *   rampChunks() as `ramp` blocks — variable-length output, so it gets its own
 *                shape. This is choreograph's core and every jog, pivot and
 *                pre-orientation is a wrapper around it; its triangular clamp
 *                and its dv==0 cruise piece are otherwise only reachable
 *                incidentally.
 *
 *   discretize() as `case` blocks — planned samples in, MicroSegments out.
 *
 *     interval <nIn> <hex…> <nOut> <hex>
 *     ramp <name> <N> <v0> <cruise> <accel> <fCpu>
 *     r <steps> <interval>
 *     end
 *     case <name>
 *     axes <xSpu xFeed xAccel> <ySpu yFeed yAccel> <zSpu zFeed zAccel> <aSpu aFeed aAccel> <fCpu>
 *     inv <xInvert> <yInvert> <zInvert> <aInvert>
 *     tool <tangential> <unwind> <cornerAngleDeg> <offsetMm>
 *     trav <dvMax> <vMin> <jogFeed> <liftHeight> <zFeed> <zAccel>
 *     slew <hasFeed> <feed> <hasAccel> <accel>
 *     n <count>
 *     i <x> <y> <theta> <kappa> <ds> <flags:int> <vCeiling> <v>
 *     o <dx> <dy> <dz> <da> <interval> <flags:int>
 *     end
 *
 * The `i` lines carry the planned samples explicitly rather than letting the
 * C++ re-derive them from the curve fixtures. Those stages are bit-verified
 * already, so re-deriving would work — but it would make a flatten regression
 * surface here as a discretize failure, and the whole value of a per-stage
 * differential is that it says which stage moved.
 *
 *     GEN_CPP_REF=1 npx vitest run test/port/cppRefDiscretize
 */

import { it } from "vitest";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import { discretize } from "../../src/toolpath/discretize.js";
import { rampChunks } from "../../src/choreograph/choreograph.js";
import { interval } from "../../src/wire/format/microsegment.js";
import { plan, type PlannedSample } from "../../src/toolpath/plan.js";
import { constrain } from "../../src/toolpath/constrain.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import {
    resolvedAxes,
    qualityConfig,
    KNIFE,
    PEN,
    type MachineConfig,
    type QualityConfig,
    type ResolvedAxes,
    type ToolProfile,
} from "../../src/config/config.js";
import { defaultConfig } from "../../src/config/fixtures.js";
import { lineToCubic, type CubicBezier } from "../../src/toolpath/geometry.js";
import { hex } from "./refFormat.js";

const HERE = dirname(fileURLToPath(import.meta.url));
const OUT = join(HERE, "../../../test/data/discretize_ref.txt");
const SVG = join(HERE, "../production/data");

const FEED = 80, A_MAX = 1000;

it("generates the C++ discretize reference", (ctx) => {
    if (process.env.GEN_CPP_REF !== "1") {
        ctx.skip();
        return;
    }

    const CFG = defaultConfig();
    const MACH = CFG.machine;
    const AXES = resolvedAxes(MACH);
    const HEAD = MACH.heads[MACH.defaultHead]!;
    const q = qualityConfig();

    const fnLines: string[] = [];
    const rampLines: string[] = [];
    const lines: string[] = [];

    const line = (a: [number, number], b: [number, number]): CubicBezier =>
        lineToCubic({ x: a[0], y: a[1] }, { x: b[0], y: b[1] });

    // ── interval() ───────────────────────────────────────────────────────────
    // A deterministic xorshift, same generator the other refs use, so the sweep
    // is reproducible without shipping the inputs twice.
    let seed = 1357911 >>> 0;
    const rnd = (): number => {
        seed ^= seed << 13; seed >>>= 0;
        seed ^= seed >>> 17;
        seed ^= seed << 5; seed >>>= 0;
        return seed / 4294967296;
    };
    const emitInterval = (v: number, ax: ResolvedAxes, vMin: number,
                          dx: number, dy: number, dz: number, da: number): void => {
        fnLines.push(["interval", 7, ...[v, vMin, dx, dy, dz, da].map(hex), hex(ax.fCpu),
                      1, hex(interval(v, ax, vMin, dx, dy, dz, da))].join(" "));
    };
    for (let i = 0; i < 2000; i++) {
        emitInterval(
            Math.pow(10, rnd() * 5 - 3),                 // v: 1e-3 … 1e2 mm/s
            AXES, q.vMin,
            Math.round((rnd() * 2 - 1) * 200),
            Math.round((rnd() * 2 - 1) * 200),
            Math.round((rnd() * 2 - 1) * 20),
            Math.round((rnd() * 2 - 1) * 400),           // A dominates often, so
        );                                               // the rate floor binds
    }
    // The branches a random sweep will not land on: no motion at all, pure Z,
    // pure A (the distMm < 1e-9 path with and without a rate floor), a v below
    // vMin, and the exact clamp endpoints.
    for (const [v, dx, dy, dz, da] of [
        [10, 0, 0, 0, 0],       // major == 0 -> fCpu
        [10, 0, 0, 5, 0],       // pure Z
        [10, 0, 0, 0, 5],       // pure A -> rate floor
        [1e-9, 1, 0, 0, 0],     // v under vMin
        [1e9, 1, 0, 0, 0],      // interval clamps to 1
        [1e-9, 1, 1, 0, 0],     // diagonal, floored
        [80, 200, 200, 0, 0],   // long diagonal, hypotenuse correction
        [80, 1, 0, 0, 400],     // A-dominated: floor beats feed
    ]) {
        emitInterval(v!, AXES, q.vMin, dx!, dy!, dz!, da!);
        emitInterval(v!, AXES, 0, dx!, dy!, dz!, da!); // vMin disabled
    }

    // ── rampChunks() ─────────────────────────────────────────────────────────
    const emitRamp = (name: string, N: number, v0: number, cruise: number,
                      accel: number, fCpu: number): void => {
        rampLines.push(`ramp ${name} ` + [N, v0, cruise, accel, fCpu].map(hex).join(" "));
        for (const c of rampChunks(N, v0, cruise, accel, fCpu)) {
            rampLines.push("r " + [c.steps, c.interval].map(hex).join(" "));
        }
        rampLines.push("end");
    };
    const F = AXES.fCpu;
    emitRamp("zero", 0, 50, 5000, 100000, F);
    emitRamp("negative", -10, 50, 5000, 100000, F);
    emitRamp("one_step", 1, 50, 5000, 100000, F);
    emitRamp("trapezoid", 20000, 50, 5000, 100000, F);   // reaches cruise
    emitRamp("triangular", 40, 50, 5000, 100000, F);     // 2*dAcc > N, clamped
    emitRamp("no_ramp", 500, 5000, 5000, 100000, F);     // dv == 0, one chunk
    emitRamp("v0_above_cruise", 500, 5000, 50, 100000, F); // dAcc negative
    emitRamp("tiny_accel", 1000, 50, 5000, 1, F);
    emitRamp("huge_accel", 1000, 50, 5000, 1e9, F);
    for (let i = 0; i < 400; i++) {
        emitRamp(`rnd${i}`,
                 Math.round(rnd() * 5000) + 1,
                 rnd() * 100,
                 rnd() * 8000 + 1,
                 rnd() * 200000 + 1,
                 F);
    }

    // ── discretize() ─────────────────────────────────────────────────────────
    const planFor = (sp: readonly (readonly CubicBezier[])[],
                     profile: ToolProfile): PlannedSample[] => {
        const s = flatten(sp, q);
        const c = constrain(s, {
            feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation,
            ...(profile.tangential
                ? { aRateDegS: AXES.a.maxFeed, aAccelDegS2: HEAD.a.maxAccel,
                    cornerStopAngleDeg: profile.cornerAngleDeg }
                : {}),
            vMin: q.vMin,
        });
        return plan(c, {
            xAccel: MACH.x.maxAccel, yAccel: MACH.y.maxAccel,
            aAccelDegS2: HEAD.a.maxAccel, aMax: A_MAX,
        });
    };

    // Input sample lists are emitted ONCE and referenced by name. Every fixture
    // is discretized under a dozen option variants, and repeating a 5000-sample
    // input beside each of them made the file 20 MB — of which 19 were the same
    // numbers over and over. The `use` indirection is not a compression trick:
    // it also makes it structurally impossible for two variants of one fixture
    // to disagree about what they were fed.
    const emitted = new Set<string>();
    const emitSamples = (key: string, input: PlannedSample[]): string => {
        if (!emitted.has(key)) {
            emitted.add(key);
            lines.push(`samples ${key}`);
            lines.push(`n ${input.length}`);
            for (const s of input) {
                lines.push("i " + [s.x, s.y, s.theta, s.kappa, s.ds].map(hex).join(" ") +
                           ` ${s.flags} ` + [s.vCeiling, s.v].map(hex).join(" "));
            }
            lines.push("end");
        }
        return key;
    };

    const emitCase = (name: string, key: string, input: PlannedSample[],
                      mach: MachineConfig, profile: ToolProfile, qual: QualityConfig,
                      overrides?: { jogFeed?: number; liftHeight?: number; zFeed?: number;
                                    zAccel?: number }): void => {
        const got = discretize(input, mach, profile, qual, overrides);
        const ax = resolvedAxes(mach);
        const targets = { rapid: mach.rapid, z: mach.z, slew: mach.slew };
        lines.push(`case ${name}`);
        lines.push(`use ${emitSamples(key, input)}`);
        lines.push("axes " + [ax.x.stepsPerUnit, ax.x.maxFeed, ax.x.maxAccel,
                              ax.y.stepsPerUnit, ax.y.maxFeed, ax.y.maxAccel,
                              ax.z.stepsPerUnit, ax.z.maxFeed, ax.z.maxAccel,
                              ax.a.stepsPerUnit, ax.a.maxFeed, ax.a.maxAccel,
                              ax.fCpu].map(hex).join(" "));
        lines.push(`inv ${+ax.x.invert} ${+ax.y.invert} ${+ax.z.invert} ${+ax.a.invert}`);
        lines.push(`tool ${+profile.tangential} ${+profile.unwind} ` +
                   [profile.cornerAngleDeg, profile.offsetMm].map(hex).join(" "));
        lines.push("trav " + [qual.dvMax, qual.vMin,
                              overrides?.jogFeed ?? targets.rapid.feed,
                              overrides?.liftHeight ?? profile.liftHeight,
                              overrides?.zFeed ?? (profile.z?.feed ?? targets.z.feed),
                              overrides?.zAccel ??
                                  (profile.z?.accel ?? targets.z.accel ?? ax.z.maxAccel)]
                                 .map(hex).join(" "));
        lines.push(`slew ${+(targets.slew.feed !== undefined)} ` +
                   `${hex(targets.slew.feed ?? 0)} ` +
                   `${+(targets.slew.accel !== undefined)} ${hex(targets.slew.accel ?? 0)}`);
        for (const m of got) {
            lines.push("o " + [m.dx, m.dy, m.dz, m.da, m.interval].map(hex).join(" ") +
                       ` ${m.flags}`);
        }
        lines.push("end");
    };

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
        full_circle_r30: [
            { p0: { x: 30, y: 0 }, p1: { x: 30, y: 16.5685 }, p2: { x: 16.5685, y: 30 }, p3: { x: 0, y: 30 } },
            { p0: { x: 0, y: 30 }, p1: { x: -16.5685, y: 30 }, p2: { x: -30, y: 16.5685 }, p3: { x: -30, y: 0 } },
            { p0: { x: -30, y: 0 }, p1: { x: -30, y: -16.5685 }, p2: { x: -16.5685, y: -30 }, p3: { x: 0, y: -30 } },
            { p0: { x: 0, y: -30 }, p1: { x: 16.5685, y: -30 }, p2: { x: 30, y: -16.5685 }, p3: { x: 30, y: 0 } },
        ],
        // The tiny tail: the subpath's final pair is a corner, so PATH_END has
        // to be re-homed past the pivot and Z-raise that follow it (D1).
        tiny_tail: [line([0, 0], [10, 0]), line([10, 0], [10.001, 0.001])],
    };

    const withAxis = (axis: "x" | "y" | "z" | "a",
                      patch: Record<string, unknown>): MachineConfig => {
        if (axis === "x" || axis === "y") {
            return { ...MACH, [axis]: { ...MACH[axis], ...patch } };
        }
        const head = { ...HEAD, [axis]: { ...HEAD[axis], ...patch } };
        return { ...MACH, heads: [head] };
    };

    // The full option sweep runs on the SMALL fixtures only. Every branch in the
    // stage is reachable on a 4 mm line, an elbow, a cusp and a tiny tail — the
    // option knobs do not care how long the path is, and sweeping a 5000-sample
    // circle through eleven variants bought nothing but 15 MB. The large
    // fixtures still get the base case and the lift, which is where their size
    // is the point (unwind accumulating over a full turn, Z pairs interleaved
    // with hundreds of cutting segments).
    const SMALL = new Set(["short_line", "diagonal", "quarter_circle_r5", "cusp",
                           "near_cusp", "elbow", "tiny_tail"]);

    for (const [name, sp] of Object.entries(geo)) {
        for (const profile of [KNIFE, PEN]) {
            const p = planFor([sp], profile);
            const tag = `${name}__${profile.name}`;
            emitCase(tag, tag, p, MACH, profile, q);
            emitCase(`${tag}__lift`, tag, p, MACH, profile, q, { liftHeight: 2.0 });
            if (!SMALL.has(name)) continue;
            // Each knob varied on its own, so a mutant that ignores one still
            // has somewhere to show up.
            emitCase(`${tag}__dense`, tag, p, MACH, profile, { ...q, dvMax: 0.05 });
            emitCase(`${tag}__coarse`, tag, p, MACH, profile, { ...q, dvMax: 1e9 });
            emitCase(`${tag}__noVMin`, tag, p, MACH, profile, { ...q, vMin: 1e-9 });
            emitCase(`${tag}__xInv`, tag, p, withAxis("x", { invert: false }), profile, q);
            emitCase(`${tag}__yInv`, tag, p, withAxis("y", { invert: true }), profile, q);
            emitCase(`${tag}__aInv`, tag, p, withAxis("a", { invert: false }), profile, q,
                     { liftHeight: 2.0 });
            emitCase(`${tag}__zInv`, tag, p, withAxis("z", { invert: false }), profile, q,
                     { liftHeight: 2.0 });
            // A non-square machine: X and Y resolutions differ, so the
            // hypotenuse correction in interval() stops being symmetric.
            emitCase(`${tag}__nonSquare`, tag, p, withAxis("y", { stepsPerUnit: 400 }), profile, q);
            // A slow A axis, so its rate floor binds far more often (D3's path).
            emitCase(`${tag}__slowA`, tag, p, withAxis("a", { maxFeed: 10 }), profile, q);
            // An explicit jog feed, the one override the fixtures never vary.
            emitCase(`${tag}__slowJog`, tag, p, MACH, profile, q, { jogFeed: 5 });
        }
    }

    // Multi-subpath: travel jogs, per-subpath pre-orientation, and the unwind
    // accumulating across subpaths rather than cancelling inside one.
    const square = [line([0, 0], [20, 0]), line([20, 0], [20, 20]),
                    line([20, 20], [0, 20]), line([0, 20], [0, 0])];
    for (const profile of [KNIFE, PEN]) {
        emitCase(`multi__${profile.name}`, `multi__${profile.name}`,
                 planFor([geo.straight_line!, geo.quarter_circle_r5!,
                          [line([200, 40], [260, 90])]], profile),
                 MACH, profile, q);
        emitCase(`squares3__${profile.name}`, `sq3__${profile.name}`, planFor([square, square, square], profile),
                 MACH, profile, q);
        emitCase(`squares3__${profile.name}__lift`, `sq3__${profile.name}`,
                 planFor([square, square, square], profile), MACH, profile, q,
                 { liftHeight: 2.0 });
        emitCase(`circles3__${profile.name}`, `c3__${profile.name}`,
                 planFor([geo.full_circle_r30!, geo.full_circle_r30!, geo.full_circle_r30!],
                         profile),
                 MACH, profile, q);
    }

    // A non-unwinding tangential tool takes preOrient's other branch entirely.
    const CREASE_LIKE: ToolProfile = { ...KNIFE, name: "crease", unwind: false,
                                       cornerAngleDeg: 30 };
    emitCase("squares3__crease", "sq3__crease", planFor([square, square, square], CREASE_LIKE),
             MACH, CREASE_LIKE, q);
    emitCase("circles3__crease", "c3__crease",
             planFor([geo.full_circle_r30!, geo.full_circle_r30!], CREASE_LIKE),
             MACH, CREASE_LIKE, q);

    // Real geometry off the golden's own fixtures. Base case only: the lift
    // path is already pinned on every synthetic fixture, and these two are what
    // make the file large.
    for (const svg of ["test_circle.svg", "fish.svg"]) {
        const { subpaths } = loadSvgMmSubpaths(readFileSync(join(SVG, svg), "utf-8"));
        const repaired = subpaths.map(
            (sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired,
        );
        const stem = svg.replace(".svg", "");
        for (const profile of [KNIFE, PEN]) {
            emitCase(`${stem}__${profile.name}`, `${stem}__${profile.name}`,
                     planFor(repaired, profile), MACH, profile, q);
        }
    }

    mkdirSync(dirname(OUT), { recursive: true });
    writeFileSync(
        OUT,
        "# GENERATED by web/test/port/cppRefDiscretize.test.ts — do not edit by hand.\n" +
        "# GEN_CPP_REF=1 npx vitest run test/port/cppRefDiscretize\n" +
        fnLines.join("\n") + "\n" + rampLines.join("\n") + "\n" + lines.join("\n") + "\n",
    );
    // eslint-disable-next-line no-console
    console.log(`wrote ${fnLines.length} interval cases, ` +
                `${rampLines.filter((l) => l.startsWith("ramp")).length} ramp cases, ` +
                `${lines.filter((l) => l.startsWith("case")).length} discretize cases, ` +
                `${lines.filter((l) => l.startsWith("o ")).length} segments`);
});
