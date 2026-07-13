/**
 * Tests for the Plan stage (redesign stage 6): look-ahead feedrate planner.
 * Ported from pipeline/stages/test_plan_lookahead.py.
 *
 * The headline property is acceleration continuity: between every adjacent
 * pair the speed change must be feasible at the segment accel, in BOTH
 * directions. The tile-era planner could not guarantee this at curve
 * junctions; this one does by construction.
 */

import { describe, it, expect } from "vitest";
import { lineToCubic, type CubicBezier } from "../../src/toolpath/geometry.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { constrain } from "../../src/toolpath/constrain.js";
import { plan, segAccel, subpathRanges, type PlannedSample } from "../../src/toolpath/plan.js";
import { CURVE_BOUNDARY } from "../../src/toolpath/sample.js";
import { CASES } from "./curves.cases.js";
import { defaultConfig, qualityConfig } from "../../src/config/config.js";

const CFG = defaultConfig();
const MACH = CFG.machine;
const HEAD = MACH.heads[MACH.defaultHead]!;
const q = qualityConfig();
const FEED = 80.0;
const A_MAX = 1000.0;

// Per-axis accel options sourced from MachineConfig at the test boundary.
const planOpts = {
    xAccel: MACH.x.accel,
    yAccel: MACH.y.accel,
    aAccelDegS2: HEAD.a.accel,
    aMax: A_MAX,
};

function line(p0: { x: number; y: number }, p1: { x: number; y: number }): CubicBezier {
    return lineToCubic(p0, p1);
}

function prep(
    subpaths: readonly (readonly CubicBezier[])[],
    aRate = 0,
    cornerStop?: number,
): PlannedSample[] {
    const s = flatten(subpaths, q);
    const c = constrain(s, {
        feedMax: FEED,
        aMax: A_MAX,
        junctionDeviation: q.junctionDeviation,
        aRateDegS: aRate,
        cornerStopAngleDeg: cornerStop,
    });
    return plan(c, planOpts);
}

function assertAccelContinuous(s: PlannedSample[]): void {
    for (const [lo, hi] of subpathRanges(s)) {
        for (let i = lo; i < hi; i++) {
            const ds = s[i]!.ds;
            const a = segAccel(s[i]!, s[i + 1]!, planOpts);
            const budget = 2 * a * ds + 1e-6;
            // forward feasible and backward feasible
            if (s[i + 1]!.v ** 2 > s[i]!.v ** 2 + budget + 1e-6) {
                throw new Error(`accel jump at ${i}`);
            }
            if (s[i]!.v ** 2 > s[i + 1]!.v ** 2 + budget + 1e-6) {
                throw new Error(`decel jump at ${i}`);
            }
        }
    }
}

// ── boundary conditions ───────────────────────────────────────────────────────

describe("stage 6: boundary conditions", () => {
    it("endpoints zero (PATH_START/PATH_END pin v=0)", () => {
        const s = prep([CASES.s_curve!.curves]);
        expect(s[0]!.v).toBe(0);
        expect(s[s.length - 1]!.v).toBe(0);
    });

    it("v within ceiling", () => {
        const s = prep([CASES.full_circle_r30!.curves]);
        for (const x of s) {
            expect(x.v).toBeLessThanOrEqual(x.vCeiling + 1e-9);
        }
    });
});

// ── the headline: acceleration continuity ─────────────────────────────────────

describe("stage 6: acceleration continuity", () => {
    it("all cases — accel-continuous in both directions", () => {
        for (const [name, { curves }] of Object.entries(CASES)) {
            const s = prep([curves], 100.0, 20.0);
            // assertAccelContinuous throws on the first violation with the sample index
            try {
                assertAccelContinuous(s);
            } catch (e) {
                throw new Error(`${name}: ${(e as Error).message}`);
            }
        }
    });

    it("multi-subpath — accel-continuous", () => {
        const s = prep([CASES.straight_line!.curves, CASES.quarter_circle_r5!.curves]);
        assertAccelContinuous(s);
    });
});

// ── shape of the profile ──────────────────────────────────────────────────────

describe("stage 6: profile shape", () => {
    it("straight line ramps up then down (100mm reaches near feed)", () => {
        const s = prep([CASES.straight_line!.curves]);
        const vs = s.map((x) => x.v);
        const peak = Math.max(...vs);
        expect(peak).toBeGreaterThan(0.9 * FEED);
        expect(vs[0]).toBe(0);
        expect(vs[vs.length - 1]).toBe(0);
        const imax = vs.indexOf(peak);
        // rising
        for (let i = 0; i < imax; i++) {
            expect(vs[i]).toBeLessThanOrEqual(vs[i + 1]! + 1e-6);
        }
        // falling
        for (let i = imax; i < vs.length - 1; i++) {
            expect(vs[i]).toBeGreaterThanOrEqual(vs[i + 1]! - 1e-6);
        }
    });

    it("short line — triangular, peak below feed", () => {
        const s = prep([CASES.short_curve!.curves]);
        const peak = Math.max(...s.map((x) => x.v));
        // 1mm line can't reach feed from rest at a_max -> triangular
        expect(peak).toBeLessThan(FEED);
        // reachable peak ~ sqrt(a_max * length) over a 1mm line, both ends at 0
        expect(peak).toBeLessThanOrEqual(Math.sqrt(A_MAX * 1.0) + 1.0);
    });
});

// ── corner stop ───────────────────────────────────────────────────────────────

describe("stage 6: corner stop", () => {
    it("corner brings both sides to zero (lift-pivot precondition)", () => {
        const horiz = line({ x: 0, y: 0 }, { x: 20, y: 0 });
        const vert = line({ x: 20, y: 0 }, { x: 20, y: 20 });
        const s = prep([[horiz, vert]], 100.0, 20.0);
        const bi = s.findIndex((x) => x.flags & CURVE_BOUNDARY);
        expect(bi).toBeGreaterThan(0);
        expect(s[bi]!.v).toBe(0);
        // coincident prior sample also ~0 (zero-gap decel)
        expect(s[bi! - 1]!.v).toBeLessThan(1.0);
    });
});

// ── A-axis tracking accel cap (term in segAccel) ──────────────────────────────

describe("stage 6: segAccel A-axis term", () => {
    it("tight arc — A-tracking cap pulls segment accel below scalar a_max", () => {
        const s = flatten([CASES.quarter_circle_r5!.curves], q);
        const i = Math.floor(s.length / 2);
        const a = segAccel(s[i]!, s[i + 1]!, planOpts);
        const kap = Math.max(s[i]!.kappa, s[i + 1]!.kappa);
        expect(a).toBeLessThan(A_MAX);
        // rad(a.accel) / kappa
        const expected = ((HEAD.a.accel * Math.PI) / 180) / kap;
        expect(Math.abs(a - expected) / a).toBeLessThan(0.1);
    });

    it("straight pure-X move — A term inactive, returns x.accel", () => {
        const s = flatten([CASES.straight_line!.curves], q);
        const i = Math.floor(s.length / 2);
        const a = segAccel(s[i]!, s[i + 1]!, planOpts);
        expect(Math.abs(a - MACH.x.accel)).toBeLessThan(1e-6);
    });
});

// ── per-axis accel ────────────────────────────────────────────────────────────

describe("stage 6: per-axis accel projection", () => {
    it("diagonal accel exceeds scalar (each axis within its limit)", () => {
        // On a 45-degree line, per-axis projection lets the tool accelerate
        // faster than the scalar a_max. Use a high feed so the cap doesn't
        // mask the accel headroom near the start.
        const diag = line({ x: 0, y: 0 }, { x: 100, y: 100 });
        const s = flatten([[diag]], q);
        const c = constrain(s, {
            feedMax: 300.0,
            aMax: A_MAX,
            junctionDeviation: q.junctionDeviation,
        });
        const p = plan(c, planOpts);
        let acc = 0;
        for (let i = 0; i < p.length - 1; i++) {
            acc += p[i]!.ds;
            if (acc > 5.0) {
                const scalarBound = Math.sqrt(2 * A_MAX * acc); // what scalar a_max gives
                expect(p[i]!.v).toBeGreaterThan(scalarBound * 1.05);
                return;
            }
        }
        throw new Error("never reached 5mm accumulation on a 100mm diagonal");
    });
});
