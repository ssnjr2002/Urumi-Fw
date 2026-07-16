/**
 * Tests for the Discretize stage (redesign stage 8): Sample stream -> MicroSegments.
 * Ported from pipeline/stages/test_discretize.py.
 *
 * The decisive check is XY conservation: the emitted net step deltas move the
 * tool exactly from the path's first sample to its last (the accumulator
 * telescopes to round(last) - round(first)), with per-axis invert applied.
 * Geometry in, correct net displacement out, regardless of segment density.
 */

import { describe, it, expect } from "vitest";
import { readFixture } from "../helpers.js";
import { lineToCubic, type CubicBezier } from "../../src/toolpath/geometry.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { constrain } from "../../src/toolpath/constrain.js";
import { plan } from "../../src/toolpath/plan.js";
import { discretize } from "../../src/toolpath/discretize.js";
import { MICRO_PATH_END, MICRO_JOG, MICRO_LIFT, type MicroSegment } from "../../src/wire/microsegment.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { CASES } from "./curves.cases.js";
import {
    defaultConfig,
    resolvedAxes,
    qualityConfig,
    KNIFE,
    PEN,
} from "../../src/config/config.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";

const CFG = defaultConfig();
const MACH = CFG.machine;
const AXES = resolvedAxes(MACH);
const HEAD = MACH.heads[MACH.defaultHead]!;
const q = qualityConfig();
const FEED = 80.0;
const A_MAX = 1000.0;

function svg(name: string): string {
    return readFixture(name);
}

function prep(
    subpaths: readonly (readonly CubicBezier[])[],
    profile: typeof KNIFE | typeof PEN,
): MicroSegment[] {
    const s = flatten(subpaths, q);
    const constrainOpts = {
        feedMax: FEED,
        aMax: A_MAX,
        junctionDeviation: q.junctionDeviation,
        ...(profile.tangential
            ? { aRateDegS: AXES.a.maxFeed, cornerStopAngleDeg: profile.cornerAngleDeg }
            : {}),
    };
    const c = constrain(s, constrainOpts);
    const p = plan(c, {
        xAccel: MACH.x.maxAccel,
        yAccel: MACH.y.maxAccel,
        aAccelDegS2: HEAD.a.maxAccel,
        aMax: A_MAX,
    });
    return discretize(p, MACH, profile, q);
}

function net(segs: readonly MicroSegment[]): [number, number, number] {
    return segs.reduce(
        (acc, s) => [acc[0] + s.dx, acc[1] + s.dy, acc[2] + s.da] as [number, number, number],
        [0, 0, 0] as [number, number, number],
    );
}

function expectedXY(subpaths: readonly (readonly CubicBezier[])[]): [number, number] {
    const s = flatten(subpaths, q);
    const xSpu = MACH.x.stepsPerUnit;
    const ySpu = MACH.y.stepsPerUnit;
    let dx = Math.round(s[s.length - 1]!.x * xSpu) - Math.round(s[0]!.x * xSpu);
    let dy = Math.round(s[s.length - 1]!.y * ySpu) - Math.round(s[0]!.y * ySpu);
    if (MACH.x.invert) dx = -dx;
    if (MACH.y.invert) dy = -dy;
    return [dx, dy];
}

// ── XY conservation: net steps land the tool at the geometric endpoint ────────

describe("stage 8: XY conservation", () => {
    it("snake SVG — net XY matches geometric endpoint", () => {
        const { subpaths } = loadSvgMmSubpaths(svg("test_snake.svg"));
        const repairOpts = { angleTolDeg: q.angleTol, gapTolMm: q.gapTol };
        const repaired = subpaths.map((sp) => enforceC1(sp, repairOpts).repaired);
        const [nx, ny] = net(prep(repaired, KNIFE));
        const [ex, ey] = expectedXY(repaired);
        expect([nx, ny]).toEqual([ex, ey]);
    });

    it("all 8 mock cases — net XY matches geometric endpoint", () => {
        for (const [name, { curves }] of Object.entries(CASES)) {
            const [nx, ny] = net(prep([curves], KNIFE));
            const [ex, ey] = expectedXY([curves]);
            if (nx !== ex || ny !== ey) {
                throw new Error(`${name}: got (${nx},${ny}) expected (${ex},${ey})`);
            }
        }
    });
});

// ── pen tool: no A rotation, no lift unless asked ─────────────────────────────

describe("stage 8: pen tool", () => {
    it("no A rotation, no MICRO_LIFT", () => {
        const segs = prep([CASES.s_curve!.curves], PEN);
        for (const s of segs) {
            expect(Math.abs(s.da)).toBe(0);
            expect(s.flags & MICRO_LIFT).toBeFalsy();
        }
    });
});

// ── path end flag ─────────────────────────────────────────────────────────────

describe("stage 8: path end flag", () => {
    it("last segment has MICRO_PATH_END", () => {
        const segs = prep([CASES.straight_line!.curves], KNIFE);
        expect(segs[segs.length - 1]!.flags & MICRO_PATH_END).toBeTruthy();
    });
});

// ── corners produce a pivot ───────────────────────────────────────────────────

describe("stage 8: corner pivot", () => {
    it("90deg corner emits pure-A MICRO_JOG pivot", () => {
        const horiz = lineToCubic({ x: 0, y: 0 }, { x: 20, y: 0 });
        const vert = lineToCubic({ x: 20, y: 0 }, { x: 20, y: 20 });
        const segs = prep([[horiz, vert]], KNIFE);
        const pivots = segs.filter(
            (s) => (s.flags & MICRO_JOG) && s.da !== 0 && s.dx === 0 && s.dy === 0,
        );
        expect(pivots.length).toBeGreaterThan(0);
    });
});

// ── unwind keeps physical A bounded ───────────────────────────────────────────

describe("stage 8: unwind bounds physical A", () => {
    it("3 full circles — peak physical A < 540 deg", () => {
        // Many small closed loops would wind A without unwind; KNIFE unwinds
        // pen-up. A single full circle: net A ~ +/-360 deg of tracking; physical
        // should stay within ~one turn since each PATH_START unwinds to the
        // entry tangent.
        const circle = CASES.full_circle_r30!.curves;
        const segs = prep([circle, circle, circle], KNIFE);
        const aInv = AXES.a.invert ? -1 : 1;
        let phys = 0;
        let peak = 0;
        for (const s of segs) {
            phys += s.da * aInv;
            peak = Math.max(peak, Math.abs(phys));
        }
        const aSpd = AXES.a.stepsPerUnit;
        expect(peak).toBeLessThan(540 * aSpd);
    });
});
