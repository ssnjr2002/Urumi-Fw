/**
 * Tests for the Plan stage (redesign stage 6): the look-ahead feedrate planner.
 *
 * Split, as with flatten and constrain (docs/planner_audit.md):
 *
 *   INVARIANTS          — must hold for every input, forever. A failure is a bug
 *                         in plan. These are what the C++ port must reproduce.
 *   CONTRACT PROPERTIES — what the stage's doc comment claims. Some of these
 *                         FAIL today; that is the point. Each failing test names
 *                         the finding it pins.
 *
 * Method notes carried over from the earlier stages:
 *
 *   - Measure independently of the implementation where possible. The
 *     accel-continuity check that recomputes `segAccel` verifies the SWEEPS but
 *     is blind to `segAccel` itself being wrong; `assertAxisAccelWithinLimits`
 *     re-derives acceleration from planned speeds and geometry alone, so it can
 *     catch what the self-consistent check cannot.
 *   - Aggregate across fixtures, then fail once (`forEachFixture`). An `expect`
 *     inside a fixture loop reports the FIRST violation and never reaches the
 *     worst one — a property test that hides its worst case behind its first is
 *     worse than no test, because it looks like it ran.
 *   - Pin exponents with scaling laws, not directions. "Goes up" passes for a
 *     dimensionally wrong formula; "4x the budget buys exactly 4x" does not.
 */

import { describe, it, expect } from "vitest";
import { lineToCubic, type CubicBezier } from "../../src/toolpath/geometry.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { constrain, type ConstrainedSample } from "../../src/toolpath/constrain.js";
import {
    plan,
    segAccel,
    subpathRanges,
    type PlanOptions,
    type PlannedSample,
} from "../../src/toolpath/plan.js";
import { CURVE_BOUNDARY, PATH_START, PATH_END, type Sample } from "../../src/toolpath/sample.js";
import { CASES, CUSP } from "./curves.cases.js";
import { qualityConfig } from "../../src/config/config.js";
import { defaultConfig } from "../../src/config/fixtures.js";

const q = qualityConfig();
const CFG = defaultConfig();
const MACH = CFG.machine;
const HEAD = MACH.heads[MACH.defaultHead]!;
const FEED = 80.0;
const A_MAX = Math.min(MACH.x.maxAccel, MACH.y.maxAccel);

const PLAN_OPTS: PlanOptions = {
    xAccel: MACH.x.maxAccel,
    yAccel: MACH.y.maxAccel,
    aAccelDegS2: HEAD.a.maxAccel,
    aMax: A_MAX,
};

// ── fixtures ──────────────────────────────────────────────────────────────────
// CUSP is deliberately outside the CASES registry (see curves.cases.ts) so a
// cusp regression is always attributable to one stage. Plan opts in here: the
// cusp is where constrain hands plan its worst input, and plan's sweeps then
// SPREAD that input over a neighbourhood — the amplification is P3 below.

const GEOMETRY_CASES: Record<string, readonly CubicBezier[]> = Object.fromEntries([
    ...Object.entries(CASES).map(([k, v]) => [k, v.curves]),
    ["cusp", CUSP],
]);

function line(p0: { x: number; y: number }, p1: { x: number; y: number }): CubicBezier {
    return lineToCubic(p0, p1);
}

function prep(
    subpaths: readonly (readonly CubicBezier[])[],
    aRate = 0,
    cornerStop?: number,
): PlannedSample[] {
    return plan(constrained(subpaths, aRate, cornerStop), PLAN_OPTS);
}

function constrained(
    subpaths: readonly (readonly CubicBezier[])[],
    aRate = 0,
    cornerStop?: number,
): ConstrainedSample[] {
    return constrain(flatten(subpaths, q), {
        feedMax: FEED,
        aMax: A_MAX,
        junctionDeviation: q.junctionDeviation,
        aRateDegS: aRate,
        aAccelDegS2: HEAD.a.maxAccel,
        cornerStopAngleDeg: cornerStop,
        // the production bridge passes this; the tests must too, or they
        // measure a pipeline nobody ships (audit C1)
        vMin: q.vMin,
    });
}

/**
 * Run `probe` over every fixture, collecting violation strings, and fail ONCE
 * with all of them. The worst fixture must not be able to hide behind the first.
 */
function forEachFixture(probe: (name: string, curves: readonly CubicBezier[]) => string[]): void {
    const violations: string[] = [];
    for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
        violations.push(...probe(name, curves));
    }
    if (violations.length > 0) {
        throw new Error(`${violations.length} violation(s):\n  ${violations.join("\n  ")}`);
    }
}

/**
 * The stage's own feasibility contract: between adjacent samples the speed
 * change must fit the segment accel budget in BOTH directions.
 *
 * Self-consistent by construction — it recomputes segAccel the same way plan
 * does, so it validates the SWEEPS, not the accel model. Pair it with
 * axisAccelViolations, which uses no plan internals at all.
 */
function feasibilityViolations(s: readonly PlannedSample[], label: string): string[] {
    const out: string[] = [];
    for (const [lo, hi] of subpathRanges(s)) {
        for (let i = lo; i < hi; i++) {
            const budget = 2 * segAccel(s[i]!, s[i + 1]!, PLAN_OPTS) * s[i]!.ds + 1e-6;
            if (s[i + 1]!.v ** 2 > s[i]!.v ** 2 + budget) {
                out.push(`${label}: accel jump at ${i}`);
            }
            if (s[i]!.v ** 2 > s[i + 1]!.v ** 2 + budget) {
                out.push(`${label}: decel jump at ${i}`);
            }
        }
    }
    return out;
}

/**
 * Total acceleration demanded of each axis by the planned profile, derived from
 * speeds and geometry ONLY — no plan internals.
 *
 * The tool's acceleration has two orthogonal components:
 *   tangential  a_t = dv/dt = (v[i+1]^2 - v[i]^2) / (2*ds)   along the tangent
 *   centripetal a_c = v^2 * kappa                             along the normal
 * Each axis must supply the projection of their VECTOR SUM. Bounding the two
 * separately (as the pipeline does) is not the same as bounding the sum.
 */
function axisAccelViolations(s: readonly PlannedSample[], label: string, tol = 1.001): string[] {
    const out: string[] = [];
    let worstX = 0;
    let worstY = 0;
    for (let i = 0; i < s.length - 1; i++) {
        const a = s[i]!;
        const b = s[i + 1]!;
        if (a.ds < 1e-9) continue;
        const aTan = (b.v * b.v - a.v * a.v) / (2 * a.ds);
        const aCen = a.v * a.v * a.kappa;
        const th = (a.theta * Math.PI) / 180;
        worstX = Math.max(worstX, Math.abs(aTan * Math.cos(th) - aCen * Math.sin(th)));
        worstY = Math.max(worstY, Math.abs(aTan * Math.sin(th) + aCen * Math.cos(th)));
    }
    if (worstX > MACH.x.maxAccel * tol) {
        out.push(`${label}: |ax| ${worstX.toFixed(1)} = ${(worstX / MACH.x.maxAccel).toFixed(3)}x x.maxAccel`);
    }
    if (worstY > MACH.y.maxAccel * tol) {
        out.push(`${label}: |ay| ${worstY.toFixed(1)} = ${(worstY / MACH.y.maxAccel).toFixed(3)}x y.maxAccel`);
    }
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════
// INVARIANTS
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 6 INVARIANT: purity and determinism", () => {
    it("does not mutate its input", () => {
        const c = constrained([CASES.s_curve!.curves], 100, 20);
        const before = JSON.stringify(c);
        plan(c, PLAN_OPTS);
        expect(JSON.stringify(c)).toBe(before);
    });

    it("is deterministic", () => {
        const c = constrained([CASES.full_circle_r30!.curves], 100, 20);
        expect(plan(c, PLAN_OPTS)).toEqual(plan(c, PLAN_OPTS));
    });

    it("preserves every sample and its geometry", () => {
        forEachFixture((name, curves) => {
            const c = constrained([curves], 100, 20);
            const p = plan(c, PLAN_OPTS);
            if (p.length !== c.length) return [`${name}: length ${c.length} -> ${p.length}`];
            const bad: string[] = [];
            for (let i = 0; i < p.length; i++) {
                const a = c[i]!;
                const b = p[i]!;
                if (a.x !== b.x || a.y !== b.y || a.theta !== b.theta ||
                    a.kappa !== b.kappa || a.ds !== b.ds || a.flags !== b.flags ||
                    a.vCeiling !== b.vCeiling) {
                    bad.push(`${name}: sample ${i} geometry/ceiling altered`);
                }
            }
            return bad.slice(0, 3);
        });
    });
});

describe("stage 6 INVARIANT: v is a sane, finite, bounded speed", () => {
    it("0 <= v <= vCeiling, finite, on every sample of every fixture", () => {
        forEachFixture((name, curves) => {
            const p = prep([curves], 100, 20);
            const bad: string[] = [];
            for (let i = 0; i < p.length; i++) {
                const v = p[i]!.v;
                if (!Number.isFinite(v)) bad.push(`${name}: sample ${i} v=${v}`);
                else if (v < 0) bad.push(`${name}: sample ${i} v=${v} < 0`);
                else if (v > p[i]!.vCeiling + 1e-9) {
                    bad.push(`${name}: sample ${i} v=${v} > ceiling ${p[i]!.vCeiling}`);
                }
            }
            return bad.slice(0, 3);
        });
    });

    it("survives the cusp without NaN (zero speed, zero ds, huge kappa)", () => {
        const p = prep([CUSP], 100, 20);
        expect(p.length).toBeGreaterThan(0);
        expect(p.every((s) => Number.isFinite(s.v))).toBe(true);
    });
});

describe("stage 6 INVARIANT: subpath endpoints are at rest", () => {
    it("every PATH_START and PATH_END sample plans to exactly 0", () => {
        forEachFixture((name, curves) => {
            // two subpaths so the pinning is exercised away from array ends too
            const p = prep([curves, [line({ x: 500, y: 500 }, { x: 560, y: 530 })]], 100, 20);
            const bad: string[] = [];
            for (let i = 0; i < p.length; i++) {
                if ((p[i]!.flags & (PATH_START | PATH_END)) && p[i]!.v !== 0) {
                    bad.push(`${name}: boundary sample ${i} v=${p[i]!.v}`);
                }
            }
            return bad;
        });
    });

    it("plans each subpath independently — a slow one does not tax its neighbour", () => {
        const fast = [line({ x: 0, y: 0 }, { x: 100, y: 0 })];
        const solo = prep([fast]);
        const paired = prep([fast, CASES.quarter_circle_r5!.curves]);
        for (let i = 0; i < solo.length; i++) {
            expect(paired[i]!.v).toBeCloseTo(solo[i]!.v, 9);
        }
    });
});

describe("stage 6 INVARIANT: the feasibility contract of the two sweeps", () => {
    it("every adjacent pair is reachable and stoppable, on every fixture", () => {
        forEachFixture((name, curves) => feasibilityViolations(prep([curves], 100, 20), name));
    });

    it("holds across multiple subpaths in one stream", () => {
        const p = prep(
            [CASES.straight_line!.curves, CASES.quarter_circle_r5!.curves, CUSP],
            100,
            20,
        );
        expect(feasibilityViolations(p, "multi")).toEqual([]);
    });

    it("holds when a corner forces a mid-subpath stop", () => {
        const p = prep([[line({ x: 0, y: 0 }, { x: 20, y: 0 }), line({ x: 20, y: 0 }, { x: 20, y: 20 })]], 100, 20);
        expect(feasibilityViolations(p, "corner")).toEqual([]);
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// CONTRACT PROPERTIES — shape of the profile
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 6: profile shape", () => {
    it("a long line ramps up, cruises at feed, ramps down", () => {
        const vs = prep([CASES.straight_line!.curves]).map((s) => s.v);
        const peak = Math.max(...vs);
        expect(peak).toBeCloseTo(FEED, 6);
        expect(vs[0]).toBe(0);
        expect(vs[vs.length - 1]).toBe(0);
        const imax = vs.indexOf(peak);
        for (let i = 0; i < imax; i++) expect(vs[i]).toBeLessThanOrEqual(vs[i + 1]! + 1e-6);
        for (let i = imax; i < vs.length - 1; i++) expect(vs[i]).toBeGreaterThanOrEqual(vs[i + 1]! - 1e-6);
    });

    it("a short line is triangular — peak is sqrt(a*L), NOT feed", () => {
        // Symmetric ramp over length L from rest to rest: the two halves meet at
        // v = sqrt(2*a*(L/2)) = sqrt(a*L). Pinning the closed form (not merely
        // "< feed") is what makes a wrong accel model fail here.
        const L = 1.0;
        const peak = Math.max(...prep([CASES.short_curve!.curves]).map((s) => s.v));
        expect(peak).toBeLessThan(FEED);
        expect(peak).toBeCloseTo(Math.sqrt(MACH.x.maxAccel * L), 0);
    });

    it("pathAccel reshapes the whole profile, not just segAccel", () => {
        // segAccel's pathAccel test is a unit check; this pins that the value
        // actually reaches the sweeps. A commanded accel of 100 mm/s^2 makes a
        // 20mm line triangular (peak sqrt(100*20) = 44.7 < feed) where the
        // machine's 1000 would have cruised at feed.
        const L = 20;
        const peakOf = (pathAccel: number) =>
            Math.max(...plan(constrained([[line({ x: 0, y: 0 }, { x: L, y: 0 })]]), {
                ...PLAN_OPTS, pathAccel,
            }).map((s) => s.v));
        expect(peakOf(0)).toBeCloseTo(FEED, 6);
        expect(peakOf(100)).toBeCloseTo(Math.sqrt(100 * L), 0);
    });

    it("4x the length of a triangular move buys exactly 2x the peak", () => {
        // peak = sqrt(a*L) — pins the exponent. A model linear in L gives 4x.
        const peakOf = (len: number) =>
            Math.max(...prep([[line({ x: 0, y: 0 }, { x: len, y: 0 })]]).map((s) => s.v));
        const p1 = peakOf(0.5);
        const p4 = peakOf(2.0);
        expect(p1).toBeLessThan(FEED);
        expect(p4).toBeLessThan(FEED);
        expect(p4 / p1).toBeCloseTo(2.0, 1);
    });

    it("a corner brings the path to rest on both sides (lift-pivot precondition)", () => {
        const p = prep([[line({ x: 0, y: 0 }, { x: 20, y: 0 }), line({ x: 20, y: 0 }, { x: 20, y: 20 })]], 100, 20);
        const bi = p.findIndex((s) => s.flags & CURVE_BOUNDARY);
        expect(bi).toBeGreaterThan(0);
        expect(p[bi]!.v).toBe(0);
        // The coincident prior sample shares the position (ds ~ 0), so the zero
        // propagates exactly, not merely "nearly" — this is what lets discretize
        // lift at a sample genuinely at rest.
        expect(p[bi - 1]!.ds).toBeLessThan(1e-9);
        expect(p[bi - 1]!.v).toBe(0);
    });
});

describe("stage 6: a constrain forcedStop becomes a real stop", () => {
    it("plans v=0 at the stop and ramps monotonically on both sides", () => {
        const samples = flatten([[line({ x: 0, y: 0 }, { x: 200, y: 0 })]], q);
        const idx = Math.floor(samples.length / 2);
        const cOpts = { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation };

        const free = plan(constrain(samples, cOpts), PLAN_OPTS);
        const stopped = plan(constrain(samples, { ...cOpts, forcedStops: new Set([idx]) }), PLAN_OPTS);

        expect(free[idx]!.v).toBeGreaterThan(0);
        expect(stopped[idx]!.v).toBe(0);
        for (let k = 1; k <= 3; k++) {
            expect(stopped[idx - k]!.v).toBeGreaterThan(stopped[idx - k + 1]!.v);
            expect(stopped[idx + k]!.v).toBeGreaterThan(stopped[idx + k - 1]!.v);
        }
        expect(stopped[0]!.v).toBeCloseTo(free[0]!.v, 9);
        expect(stopped[stopped.length - 1]!.v).toBeCloseTo(free[free.length - 1]!.v, 9);
    });

    it("the stop costs budget: the ramp reaches back exactly v^2/2a", () => {
        // docs/tool_duty_limits.md §5 — "the break costs budget". The distance
        // the stop steals is the decel distance, and tier 2's window arithmetic
        // depends on it being this and not something else.
        const samples = flatten([[line({ x: 0, y: 0 }, { x: 200, y: 0 })]], q);
        const idx = Math.floor(samples.length / 2);
        const cOpts = { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation };
        const stopped = plan(constrain(samples, { ...cOpts, forcedStops: new Set([idx]) }), PLAN_OPTS);

        let dist = 0;
        for (let i = idx - 1; i >= 0 && stopped[i]!.v < FEED - 1e-6; i--) dist += stopped[i]!.ds;
        // Sample-quantised: the ramp's true start falls inside a segment, so the
        // measured span is short by up to one dsMax. Tolerance is that spacing,
        // not a fudge factor — tightening it further would only pin flatten's
        // sample placement, which is not this stage's contract.
        const exact = (FEED * FEED) / (2 * MACH.x.maxAccel);
        expect(dist).toBeGreaterThan(exact - q.dsMax);
        expect(dist).toBeLessThanOrEqual(exact + q.dsMax);
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// CONTRACT PROPERTIES — monotonicity
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 6: monotonicity — more budget never plans slower", () => {
    function assertNeverLowers(label: string, tight: PlanOptions, loose: PlanOptions): void {
        forEachFixture((name, curves) => {
            const c = constrained([curves], 100, 20);
            const a = plan(c, tight);
            const b = plan(c, loose);
            const bad: string[] = [];
            for (let i = 0; i < a.length; i++) {
                if (b[i]!.v < a[i]!.v - 1e-9) {
                    bad.push(`${label} ${name}: sample ${i} ${a[i]!.v} -> ${b[i]!.v}`);
                }
            }
            return bad.slice(0, 3);
        });
    }

    it("raising xAccel never lowers any planned speed", () => {
        assertNeverLowers("xAccel", PLAN_OPTS, { ...PLAN_OPTS, xAccel: PLAN_OPTS.xAccel * 4 });
    });

    it("raising yAccel never lowers any planned speed", () => {
        assertNeverLowers("yAccel", PLAN_OPTS, { ...PLAN_OPTS, yAccel: PLAN_OPTS.yAccel * 4 });
    });

    it("raising aAccelDegS2 never lowers any planned speed", () => {
        assertNeverLowers("aAccel", PLAN_OPTS, { ...PLAN_OPTS, aAccelDegS2: PLAN_OPTS.aAccelDegS2 * 4 });
    });

    it("raising pathAccel never lowers any planned speed", () => {
        assertNeverLowers(
            "pathAccel",
            { ...PLAN_OPTS, pathAccel: 100 },
            { ...PLAN_OPTS, pathAccel: 400 },
        );
    });

    it("a higher vCeiling never lowers planned speed where there is no curvature", () => {
        // This used to hold unconditionally. It cannot any more, and the reason
        // is the P1 fix rather than a regression: the accel budget is now
        // SHARED, so a ceiling that lets the tool take a curve faster really
        // does leave less acceleration to speed up alongside it. Where kappa is
        // 0 there is no centripetal term, nothing to share, and the original
        // property still holds exactly.
        const c = constrained([[line({ x: 0, y: 0 }, { x: 100, y: 0 })]], 100, 20);
        const lifted = c.map((s) => ({ ...s, vCeiling: s.vCeiling * 2 }));
        const a = plan(c, PLAN_OPTS);
        const b = plan(lifted, PLAN_OPTS);
        for (let i = 0; i < a.length; i++) expect(b[i]!.v).toBeGreaterThanOrEqual(a[i]!.v - 1e-9);
    });

    it("on curved geometry the coupling is real, bounded, and only at kappa > 0", () => {
        // The other half: where it does lower a speed, that must be explained by
        // curvature and must be small. An unbounded or kappa-free regression
        // here would mean the headroom term is wrong, not merely conservative.
        forEachFixture((name, curves) => {
            const c = constrained([curves], 100, 20);
            const lifted = c.map((s) => ({ ...s, vCeiling: s.vCeiling * 2 }));
            const a = plan(c, PLAN_OPTS);
            const b = plan(lifted, PLAN_OPTS);
            const bad: string[] = [];
            for (let i = 0; i < a.length; i++) {
                if (b[i]!.v < a[i]!.v - 1e-9 && c[i]!.kappa <= 1e-9) {
                    bad.push(`${name}: sample ${i} slowed with kappa=0`);
                }
            }
            return bad.slice(0, 3);
        });
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// segAccel — the per-segment acceleration model
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 6: segAccel", () => {
    const flat = (x: number, y: number, kappa = 0): Sample =>
        ({ x, y, theta: 0, kappa, ds: 1, flags: 0 });
    const noA: PlanOptions = { ...PLAN_OPTS, aAccelDegS2: 0 };

    it("pure-X move is bounded by xAccel alone", () => {
        expect(segAccel(flat(0, 0), flat(1, 0), noA)).toBeCloseTo(MACH.x.maxAccel, 6);
    });

    it("pure-Y move is bounded by yAccel alone", () => {
        expect(segAccel(flat(0, 0), flat(0, 1), noA)).toBeCloseTo(MACH.y.maxAccel, 6);
    });

    it("pure-Y move uses yAccel, not the aMax fallback (non-square machine)", () => {
        // The test above cannot tell those apart: the default config is square,
        // so yAccel == aMax and dropping the Y candidate returns the same
        // number via the fallback. Pull them apart explicitly.
        const o: PlanOptions = { ...noA, yAccel: 250, aMax: 1000 };
        expect(segAccel(flat(0, 0), flat(0, 1), o)).toBeCloseTo(250, 6);
    });

    it("pure-X move uses xAccel, not the aMax fallback (non-square machine)", () => {
        const o: PlanOptions = { ...noA, xAccel: 250, aMax: 1000 };
        expect(segAccel(flat(0, 0), flat(1, 0), o)).toBeCloseTo(250, 6);
    });

    it("a 45-degree diagonal allows exactly sqrt(2) times the axis limit", () => {
        // Each axis supplies a/sqrt(2); both are at their limit when a = sqrt(2)*limit.
        // This is the whole reason for per-axis projection and it is an EXACT
        // number, not a "greater than the scalar" — pin it as one.
        const a = segAccel(flat(0, 0), flat(1, 1), noA);
        expect(a).toBeCloseTo(Math.SQRT2 * A_MAX, 6);
    });

    it("a degenerate (zero-length) segment falls back to the scalar aMax", () => {
        expect(segAccel(flat(5, 5), flat(5, 5), noA)).toBe(A_MAX);
    });

    it("an unlimited (0) axis does not constrain", () => {
        const a = segAccel(flat(0, 0), flat(1, 0), { ...noA, xAccel: 0 });
        expect(a).toBe(A_MAX); // no candidate applies -> scalar fallback
    });

    it("the A term is rad(aAccel)/kappa — 4x aAccel buys exactly 4x", () => {
        const s0 = flat(0, 0, 0.2);
        const s1 = flat(0.01, 0, 0.2);
        const a1 = segAccel(s0, s1, { ...PLAN_OPTS, xAccel: 0, yAccel: 0, aAccelDegS2: 50 });
        const a4 = segAccel(s0, s1, { ...PLAN_OPTS, xAccel: 0, yAccel: 0, aAccelDegS2: 200 });
        expect(a1).toBeCloseTo((50 * Math.PI) / 180 / 0.2, 6);
        expect(a4 / a1).toBeCloseTo(4.0, 6);
    });

    it("the A term is inverse-LINEAR in kappa — 10x kappa costs exactly 10x", () => {
        // Distinguishes rad(a)/k from the sqrt(a/k) SHAPE used by constrain's
        // centripetal cap. Pasting one branch into the other survives a
        // "goes down" assertion; it does not survive this.
        const o = { ...PLAN_OPTS, xAccel: 0, yAccel: 0, aAccelDegS2: 100 };
        const a1 = segAccel(flat(0, 0, 0.02), flat(0.01, 0, 0.02), o);
        const a10 = segAccel(flat(0, 0, 0.2), flat(0.01, 0, 0.2), o);
        expect(a1 / a10).toBeCloseTo(10.0, 6);
    });

    it("the A term uses the LARGER kappa of the pair (the conservative one)", () => {
        const o = { ...PLAN_OPTS, xAccel: 0, yAccel: 0, aAccelDegS2: 100 };
        const mixed = segAccel(flat(0, 0, 0.02), flat(0.01, 0, 0.2), o);
        const both = segAccel(flat(0, 0, 0.2), flat(0.01, 0, 0.2), o);
        expect(mixed).toBeCloseTo(both, 9);
    });

    it("never exceeds what either endpoint's kappa alone would allow", () => {
        // The property behind "use the larger kappa": on real geometry, taking
        // the min would let a segment leaving a curvature spike accelerate as if
        // it were already straight. Stated as a bound rather than a formula, so
        // it holds for any conservative choice.
        forEachFixture((name, curves) => {
            const s = flatten([curves], q);
            const bad: string[] = [];
            for (let i = 0; i < s.length - 1; i++) {
                const pair = segAccel(s[i]!, s[i + 1]!, PLAN_OPTS);
                const a0 = segAccel({ ...s[i]! }, { ...s[i + 1]!, kappa: s[i]!.kappa }, PLAN_OPTS);
                const a1 = segAccel({ ...s[i]!, kappa: s[i + 1]!.kappa }, s[i + 1]!, PLAN_OPTS);
                if (pair > Math.min(a0, a1) * (1 + 1e-9)) {
                    bad.push(`${name}: seg ${i} ${pair} > min(${a0}, ${a1})`);
                }
            }
            return bad.slice(0, 3);
        });
    });

    it("a zero kappa skips the A term entirely", () => {
        const o = { ...PLAN_OPTS, aAccelDegS2: 1 }; // absurdly tight if applied
        expect(segAccel(flat(0, 0, 0), flat(1, 0, 0), o)).toBeCloseTo(MACH.x.maxAccel, 6);
    });

    it("pathAccel caps the per-axis-derived limit", () => {
        const a = segAccel(flat(0, 0), flat(1, 1), { ...noA, pathAccel: 100 });
        expect(a).toBe(100);
    });

    it("pathAccel above the derived limit changes nothing (byte-neutral when slack)", () => {
        const bare = segAccel(flat(0, 0), flat(1, 1), noA);
        expect(segAccel(flat(0, 0), flat(1, 1), { ...noA, pathAccel: 1e6 })).toBe(bare);
        expect(segAccel(flat(0, 0), flat(1, 1), { ...noA, pathAccel: 0 })).toBe(bare);
    });

    it("is symmetric in its two samples", () => {
        forEachFixture((name, curves) => {
            const s = flatten([curves], q);
            const bad: string[] = [];
            for (let i = 0; i < s.length - 1; i++) {
                const f = segAccel(s[i]!, s[i + 1]!, PLAN_OPTS);
                const r = segAccel(s[i + 1]!, s[i]!, PLAN_OPTS);
                if (Math.abs(f - r) > 1e-9 * Math.max(1, f)) bad.push(`${name}: seg ${i} ${f} vs ${r}`);
            }
            return bad.slice(0, 3);
        });
    });

    it("is always positive and finite, on every segment of every fixture", () => {
        forEachFixture((name, curves) => {
            const s = flatten([curves], q);
            const bad: string[] = [];
            for (let i = 0; i < s.length - 1; i++) {
                const a = segAccel(s[i]!, s[i + 1]!, PLAN_OPTS);
                if (!Number.isFinite(a) || a <= 0) bad.push(`${name}: seg ${i} a=${a}`);
            }
            return bad.slice(0, 3);
        });
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// subpathRanges
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 6: subpathRanges", () => {
    const s = (flags: number): Sample => ({ x: 0, y: 0, theta: 0, kappa: 0, ds: 0, flags });

    it("yields one inclusive range per PATH_START..PATH_END", () => {
        const st = [s(PATH_START), s(0), s(PATH_END), s(PATH_START), s(PATH_END)];
        expect([...subpathRanges(st)]).toEqual([[0, 2], [3, 4]]);
    });

    it("handles a single-sample subpath (START and END on one sample)", () => {
        expect([...subpathRanges([s(PATH_START | PATH_END)])]).toEqual([[0, 0]]);
    });

    it("covers every sample flatten produces, with no gaps or overlaps", () => {
        forEachFixture((name, curves) => {
            const st = flatten([curves, [line({ x: 300, y: 0 }, { x: 340, y: 20 })]], q);
            const covered = new Array<number>(st.length).fill(0);
            for (const [lo, hi] of subpathRanges(st)) {
                for (let i = lo; i <= hi; i++) covered[i]!++;
            }
            const bad: string[] = [];
            for (let i = 0; i < covered.length; i++) {
                if (covered[i] !== 1) bad.push(`${name}: sample ${i} covered ${covered[i]}x`);
            }
            return bad.slice(0, 3);
        });
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// FINDINGS — these fail. Each pins a defect recorded in docs/planner_audit.md.
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 6 P1 (FIXED): the axis accel budget is one budget", () => {
    // constrain bounds the CENTRIPETAL accel by aMax (v <= sqrt(aMax/kappa));
    // plan bounds the TANGENTIAL accel by the per-axis projection. The two are
    // orthogonal, so an axis could be asked for up to sqrt(2)*aMax = 1414
    // mm/s^2 against a 1000 mm/s^2 limit, and was: 1412 on `cusp`. Neither
    // stage was wrong in isolation; nothing owned the sum.
    //
    // plan now spends one budget: each segment's tangential allowance is
    // reduced by the centripetal load already committed there,
    // a_t <= sqrt(aMax^2 - a_c^2). Measured cost in path time: +0.9% on `cusp`,
    // under +0.3% on every other fixture.
    it("no axis is asked for more acceleration than it has", () => {
        forEachFixture((name, curves) => axisAccelViolations(prep([curves], 100, 20), name));
    });

    it("the cusp no longer reaches ~sqrt(2) x aMax", () => {
        // The measurement that sized P1, inverted. It was 1412 against a 1000
        // limit — 1.41x, essentially the analytic worst case.
        const p = prep([CUSP], 100, 20);
        let worst = 0;
        for (let i = 0; i < p.length - 1; i++) {
            if (p[i]!.ds < 1e-9) continue;
            const aTan = (p[i + 1]!.v ** 2 - p[i]!.v ** 2) / (2 * p[i]!.ds);
            worst = Math.max(worst, Math.hypot(aTan, p[i]!.v ** 2 * p[i]!.kappa));
        }
        expect(worst).toBeLessThanOrEqual(A_MAX * 1.001);
    });
});

describe("stage 6 P2: an unbracketed stream is refused, not silently unplanned", () => {
    // subpathRanges yields nothing for a stream without PATH_START/PATH_END, so
    // the sweeps never ran, the endpoints were never pinned, and plan returned
    // v = vCeiling verbatim: full feed from a standing start, no error.
    //
    // flatten always brackets, so production was safe. But plan is an exported
    // pure stage taking arbitrary ConstrainedSample[], and the port gives it
    // callers (jog, streamed tiles) that are not flatten.
    const bare = () =>
        constrain(
            flatten([[line({ x: 0, y: 0 }, { x: 100, y: 0 })]], q).map((s) => ({ ...s, flags: 0 })),
            { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation },
        );

    it("throws on a stream with no PATH_START/PATH_END at all", () => {
        expect(() => plan(bare(), PLAN_OPTS)).toThrow(/outside any PATH_START\/PATH_END bracket/);
    });

    it("throws on a subpath whose PATH_END is missing, naming the cause", () => {
        const st = flatten([[line({ x: 0, y: 0 }, { x: 100, y: 0 })]], q)
            .map((s, i, arr) => (i === arr.length - 1 ? { ...s, flags: 0 } : s));
        const c = constrain(st, { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation });
        expect(() => plan(c, PLAN_OPTS)).toThrow(/missing PATH_END/);
        expect([...subpathRanges(st)].length).toBe(0); // the silence plan now refuses
    });

    it("throws on a gap BETWEEN two otherwise well-formed subpaths", () => {
        // The check is coverage, not just termination: an unbracketed sample
        // sandwiched between good subpaths was equally invisible.
        const a = flatten([[line({ x: 0, y: 0 }, { x: 10, y: 0 })]], q);
        const b = flatten([[line({ x: 20, y: 0 }, { x: 30, y: 0 })]], q);
        const gap = [...a, { ...a[a.length - 1]!, flags: 0 }, ...b];
        const c = constrain(gap, { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation });
        expect(() => plan(c, PLAN_OPTS)).toThrow(/outside any PATH_START\/PATH_END bracket/);
    });

    it("still accepts what flatten actually emits, single and multi subpath", () => {
        const one = flatten([[line({ x: 0, y: 0 }, { x: 10, y: 0 })]], q);
        const two = [...one, ...flatten([[line({ x: 20, y: 0 }, { x: 30, y: 0 })]], q)];
        for (const st of [one, two]) {
            const c = constrain(st, { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation });
            expect(() => plan(c, PLAN_OPTS)).not.toThrow();
        }
    });

    it("accepts an empty stream", () => {
        expect(plan([], PLAN_OPTS)).toEqual([]);
    });
});

describe("stage 6 FINDING P3: plan carries unexecutable ceilings straight through", () => {
    // docs/planner_audit.md C1: constrain can emit a vCeiling far below
    // quality.vMin (3.24e-3 mm/s at a cusp), which discretize then clamps up to
    // vMin — so the planned timeline and the executed one diverge.
    //
    // MEASURED, against the expectation: plan does NOT amplify this. The count
    // of below-vMin samples is IDENTICAL before and after planning (near_cusp:
    // 72 ceilings, 72 speeds). The sweeps only ever lower v, and around a
    // curvature spike the ceiling is already the binding constraint, so plan is
    // a faithful conduit rather than a spreader.
    //
    // That is the useful part of this finding: it localises the fix. A vMin
    // floor belongs in constrain, where the ceiling is set — adding one here
    // would paper over the same numbers one stage later.
    // Severity is measured as ARC LENGTH spent below vMin, not a count of
    // samples. Counting is the wrong instrument: every ramp from rest crosses
    // (0, vMin) on its way up, so a sample landing in the band is sometimes
    // legitimate — a marginal one shows up on the cusp fixture at v = 0.498.
    // The distance an honest ramp crossing costs is bounded and tiny:
    //     vMin^2 / 2a = 0.5^2 / 2000 = 1.25e-4 mm
    // Measured against that, the fixtures separate completely: every
    // well-behaved case spends exactly 0, the two cusps spend 540x and 887x.
    const RAMP_SPAN = (q.vMin * q.vMin) / (2 * A_MAX);

    /**
     * Maximal runs of samples with 0 < v < vMin, as [firstIndex, lastIndex].
     */
    function subVMinRuns(p: readonly { v: number }[]): [number, number][] {
        const runs: [number, number][] = [];
        let start = -1;
        for (let i = 0; i < p.length; i++) {
            const below = p[i]!.v > 0 && p[i]!.v < q.vMin;
            if (below && start < 0) start = i;
            if (!below && start >= 0) { runs.push([start, i - 1]); start = -1; }
        }
        if (start >= 0) runs.push([start, p.length - 1]);
        return runs;
    }

    it("every below-vMin stretch is a ramp out of a stop, not a crawl", () => {
        // The instrument changed with C1, and the reason is worth stating.
        //
        // The old bound was an arc length: vMin^2 / 2a with a = A_MAX. That was
        // right when the question was "is the tool crawling at 3e-3 mm/s", and
        // it is wrong now, because it assumes the NOMINAL acceleration. Near a
        // cusp the locally available accel is a small fraction of A_MAX (the A
        // cap, and now P1's shared budget), so ramping out of a stop honestly
        // spends far more than vMin^2/2*A_MAX in the band — 2.45e-2 mm on the
        // cusp, which the old bound called a 196x violation and which is simply
        // arithmetic.
        //
        // What actually distinguishes the defect from the arithmetic is WHERE
        // the slow samples are. Ramping through (0, vMin) on the way out of a
        // stop is unavoidable. Sitting below vMin in the middle of a moving
        // stretch is C1's crawl. So: every below-vMin run must touch a full
        // stop at one end.
        forEachFixture((name, curves) => {
            const p = prep([curves], 100, 20);
            const bad: string[] = [];
            for (const [a, b] of subVMinRuns(p)) {
                const touchesStop = (a > 0 && p[a - 1]!.v === 0) || (b < p.length - 1 && p[b + 1]!.v === 0);
                if (!touchesStop) {
                    const slowest = Math.min(...p.slice(a, b + 1).map((x) => x.v));
                    bad.push(`${name}: samples ${a}-${b} sit below vMin with no stop at either end, slowest ${slowest.toExponential(2)} mm/s`);
                }
            }
            return bad.slice(0, 3);
        });
    });

    it("introduces no unexecutable speed of its OWN beyond a ramp crossing", () => {
        // Fails on `cusp` only, at 1.40e-2 mm of the 6.75e-2 mm total — about a
        // fifth. So plan DOES spread C1 into samples whose own ceiling was
        // healthy, by decelerating into and accelerating out of the spike, but
        // the bulk of the damage is inherited rather than created.
        //
        // Splitting inherited from self-inflicted is what decides where the fix
        // goes: a vMin floor in constrain removes the four fifths, and the
        // remaining fifth disappears with it because the sweeps will have
        // nothing pathological left to ramp towards. Neither number argues for
        // clamping here, which would only hide the divergence one stage later.
        forEachFixture((name, curves) => {
            const c = constrained([curves], 100, 20);
            const p = plan(c, PLAN_OPTS);
            const bad: string[] = [];
            for (const [a, b] of subVMinRuns(p)) {
                const touchesStop = (a > 0 && p[a - 1]!.v === 0) || (b < p.length - 1 && p[b + 1]!.v === 0);
                if (touchesStop) continue; // an honest ramp crossing
                for (let i = a; i <= b; i++) {
                    if (c[i]!.vCeiling === 0 || c[i]!.vCeiling >= q.vMin) {
                        bad.push(`${name}: sample ${i} below vMin from a healthy ceiling`);
                        break;
                    }
                }
            }
            return bad.slice(0, 3);
        });
    });
});

describe("stage 6 FINDING P4: a non-tangential tool still pays the A-axis cap", () => {
    // compileBlock passes axes.a.maxAccel to plan UNCONDITIONALLY, while it
    // gates the same axis's limits to constrain on profile.tangential. So a pen
    // — which is not tracking the tangent at all — has its path acceleration cut
    // by rad(aAccel)/kappa on every curve. The asymmetry is called deliberate in
    // compileBlock's header; the cost is not stated there.
    it("costs a non-tangential tool a large factor on a tight arc", () => {
        const s = flatten([CASES.quarter_circle_r5!.curves], q);
        let worst = Infinity;
        for (let i = 0; i < s.length - 1; i++) {
            const withA = segAccel(s[i]!, s[i + 1]!, PLAN_OPTS);
            const without = segAccel(s[i]!, s[i + 1]!, { ...PLAN_OPTS, aAccelDegS2: 0 });
            worst = Math.min(worst, withA / without);
        }
        // Documented, not asserted as correct: on a 5mm arc the A term cuts the
        // available path accel to ~1/8. If P4 is resolved by gating on
        // tangential, this test moves to compileBlock where the gate lives.
        expect(worst).toBeLessThan(0.2);
    });
});
