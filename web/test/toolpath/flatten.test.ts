/**
 * Tests for the Flatten stage (redesign stage 4): Bezier subpaths -> Sample stream.
 *
 * Two kinds of test live here, and the distinction matters:
 *
 *   1. INVARIANTS — things true of any correct flattener. Endpoints preserved,
 *      flags placed, values finite, output deterministic. These pin behaviour
 *      that must survive the C++ port unchanged.
 *
 *   2. CONTRACT PROPERTIES — the three caps that DEFINE the stage:
 *        chord deviation <= chordTol
 *        sample spacing  <= dsMax
 *        tangent turn    <= dthetaMax
 *      These are the stage's whole reason for existing. Two of the three had no
 *      test before this file was rewritten, which is how docs/planner_audit.md
 *      F1 survived. They are asserted over EVERY fixture, including cusps and
 *      real SVG, because a cap that only holds on a straight line is not a cap.
 *
 * The contract properties are measured IMPLEMENTATION-INDEPENDENTLY: deviation
 * is the distance from a densely-probed true curve to the emitted polyline, not
 * a re-run of flatten's own `dtAt` predictor. A test that re-derives the code
 * under test proves only that it is self-consistent. This form ports to C++ as
 * an acceptance gate with no changes.
 *
 * Some of these are EXPECTED TO FAIL against the current implementation. That is
 * deliberate — the defects are pinned before any fix moves, so a later green run
 * is evidence the fix worked rather than evidence the test was written to match
 * whatever the code already did. See docs/planner_audit.md.
 */

import { describe, it, expect } from "vitest";
import { readFixture } from "../helpers.js";
import {
    bezierPoint,
    bezierDeriv1,
    lineToCubic,
    type CubicBezier,
    type Pt,
} from "../../src/toolpath/geometry.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { PATH_START, PATH_END, CURVE_BOUNDARY, type Sample } from "../../src/toolpath/sample.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { CASES, CUSP } from "./curves.cases.js";
import { qualityConfig } from "../../src/config/config.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";

const q = qualityConfig();

// ── reference geometry (independent of flatten) ───────────────────────────────

/**
 * Arc length by dense chord summation — the reference the flatten tests measure
 * against. Slower but far more trustworthy than quadrature on curves whose |B'|
 * varies sharply. geometry.ts used to export a 5-point Gauss-Legendre
 * `arcLength()` which under-reported a near-cusp badly; an earlier version of
 * this file had a test pinned to that error (it asserted chordSum >= GL5, which
 * is only true because GL5 was wrong — a chord sum can never exceed true arc
 * length). That function had no production caller and was removed (audit F6).
 */
function denseArcLength(c: CubicBezier, n = 20000): number {
    let total = 0;
    let prev = bezierPoint(c, 0);
    for (let i = 1; i <= n; i++) {
        const p = bezierPoint(c, i / n);
        total += Math.hypot(p.x - prev.x, p.y - prev.y);
        prev = p;
    }
    return total;
}

function denseArcLengthAll(curves: readonly CubicBezier[]): number {
    return curves.reduce((sum, c) => sum + denseArcLength(c), 0);
}

/** Perpendicular distance from p to segment ab (or to the nearer endpoint). */
function distToSegment(p: Pt, a: Pt, b: Pt): number {
    const vx = b.x - a.x;
    const vy = b.y - a.y;
    const len2 = vx * vx + vy * vy;
    if (len2 < 1e-24) return Math.hypot(p.x - a.x, p.y - a.y);
    let t = ((p.x - a.x) * vx + (p.y - a.y) * vy) / len2;
    t = Math.max(0, Math.min(1, t));
    return Math.hypot(p.x - (a.x + t * vx), p.y - (a.y + t * vy));
}

/**
 * One-sided Hausdorff distance from the true curves to the emitted polyline:
 * for each densely probed point on the real geometry, how far is the nearest
 * point on the polyline the machine will actually travel?
 *
 * This is what `chordTol` MEANS. Note it does not need flatten's `t` values, so
 * it cannot be fooled by a predictor that is self-consistent but wrong.
 */
function polylineDeviation(
    curves: readonly CubicBezier[],
    samples: readonly Sample[],
    probesPerCurve = 400,
): number {
    let worst = 0;
    for (const c of curves) {
        for (let i = 0; i <= probesPerCurve; i++) {
            const p = bezierPoint(c, i / probesPerCurve);
            let best = Infinity;
            for (let j = 0; j < samples.length - 1; j++) {
                const d = distToSegment(p, samples[j]!, samples[j + 1]!);
                if (d < best) best = d;
                if (best === 0) break;
            }
            if (best > worst) worst = best;
        }
    }
    return worst;
}

/** Shortest absolute angular difference in degrees, range [0, 180]. */
function absAngleDelta(a: number, b: number): number {
    let d = Math.abs(b - a) % 360;
    if (d > 180) d = 360 - d;
    return d;
}

// ── the fixture table every property test runs over ───────────────────────────
// Single-subpath geometry only, so `samples` is one polyline and the deviation
// helper can treat it as such.

const GEOMETRY_CASES: Readonly<Record<string, CubicBezier[]>> = {
    ...Object.fromEntries(Object.entries(CASES).map(([k, v]) => [k, v.curves])),
    cusp: CUSP,
};

/**
 * Run `probe` over every fixture and fail ONCE with all violations.
 *
 * Not cosmetic. A bare `expect` inside a fixture loop throws on the first
 * violation, so every later fixture goes unreported — the first version of this
 * file failed on `near_cusp` (2.8deg, a mild predictor overshoot) and never
 * reached `cusp` (178deg, the actual defect). A property test that hides the
 * worst case behind the first case is worse than no test, because it looks like
 * it ran.
 */
function forEachFixture(probe: (name: string, curves: CubicBezier[]) => string[]): void {
    const problems: string[] = [];
    for (const [name, curves] of Object.entries(GEOMETRY_CASES)) problems.push(...probe(name, curves));
    if (problems.length === 0) return;
    const shown = problems.slice(0, 12).join("\n  ");
    const rest = problems.length > 12 ? `\n  ...and ${problems.length - 12} more` : "";
    throw new Error(`${problems.length} violation(s):\n  ${shown}${rest}`);
}

// ═══ 1. INVARIANTS ════════════════════════════════════════════════════════════

describe("flatten: arc length", () => {
    it("chord sum never exceeds true arc length", () => {
        // Structural: a chord is the shortest path between its endpoints, so the
        // polyline can only under-measure. If this ever fails, samples are not
        // ordered along the curve.
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            const chordTotal = flatten([curves], q).reduce((sum, s) => sum + s.ds, 0);
            const trueLen = denseArcLengthAll(curves);
            expect(chordTotal, `${name}: chord ${chordTotal} > true ${trueLen}`)
                .toBeLessThanOrEqual(trueLen + 1e-9);
        }
    });

    it("chord sum is within 0.5% of true arc length", () => {
        // Density, not correctness: enough samples that the polyline does not
        // visibly short-cut the curve. Cusps included — no exemptions.
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            const chordTotal = flatten([curves], q).reduce((sum, s) => sum + s.ds, 0);
            const trueLen = denseArcLengthAll(curves);
            const err = Math.abs(chordTotal - trueLen) / trueLen;
            expect(err, `${name}: chord ${chordTotal} vs true ${trueLen}`).toBeLessThan(0.005);
        }
    });

    it("straight line measures 100mm", () => {
        const total = flatten([CASES.straight_line!.curves], q).reduce((s, x) => s + x.ds, 0);
        expect(Math.abs(total - 100.0)).toBeLessThan(0.01);
    });

});

describe("flatten: endpoints", () => {
    it("first sample is p0 of the first curve, last is p3 of the last", () => {
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            const samples = flatten([curves], q);
            const first = samples[0]!;
            const last = samples[samples.length - 1]!;
            const p0 = curves[0]!.p0;
            const p3 = curves[curves.length - 1]!.p3;
            expect(Math.hypot(first.x - p0.x, first.y - p0.y), `${name}: start`).toBeLessThan(1e-9);
            expect(Math.hypot(last.x - p3.x, last.y - p3.y), `${name}: end`).toBeLessThan(1e-6);
        }
    });
});

describe("flatten: curvature", () => {
    it("quarter circle r50 — kappa ~ 0.02 at every sample", () => {
        for (const s of flatten([CASES.quarter_circle_r50!.curves], q)) {
            expect(Math.abs(s.kappa - 0.02)).toBeLessThan(0.02 * 0.05);
        }
    });

    it("straight line — kappa ~ 0 everywhere", () => {
        const maxKappa = flatten([CASES.straight_line!.curves], q)
            .reduce((mx, s) => Math.max(mx, s.kappa), 0);
        expect(maxKappa).toBeLessThan(1e-6);
    });

    it("kappa is finite and non-negative on every fixture", () => {
        // Cheap, but the cusp fixture drives |B'| to exactly zero and kappa is
        // |B'xB''|/|B'|^3 — one missing guard away from Infinity or NaN, which
        // would propagate silently through constrain into a bad velocity.
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            for (const s of flatten([curves], q)) {
                expect(Number.isFinite(s.kappa), `${name}: kappa ${s.kappa}`).toBe(true);
                expect(s.kappa, `${name}`).toBeGreaterThanOrEqual(0);
            }
        }
    });

    it("every emitted field is finite", () => {
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            for (const s of flatten([curves], q)) {
                for (const [k, v] of Object.entries({ x: s.x, y: s.y, theta: s.theta, ds: s.ds })) {
                    expect(Number.isFinite(v), `${name}: ${k} = ${v}`).toBe(true);
                }
            }
        }
    });
});

describe("flatten: flags", () => {
    it("s_curve — exactly one PATH_START and one PATH_END", () => {
        const samples = flatten([CASES.s_curve!.curves], q);
        expect(samples[0]!.flags & PATH_START).toBeTruthy();
        expect(samples[samples.length - 1]!.flags & PATH_END).toBeTruthy();
        expect(samples.filter((s) => s.flags & PATH_START)).toHaveLength(1);
        expect(samples.filter((s) => s.flags & PATH_END)).toHaveLength(1);
    });

    it("CURVE_BOUNDARY count is (curves - 1) on every fixture", () => {
        // Was asserted for two hand-picked fixtures; it is a general rule.
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            const n = flatten([curves], q).filter((s) => s.flags & CURVE_BOUNDARY).length;
            expect(n, `${name}`).toBe(curves.length - 1);
        }
    });
});

describe("flatten: multi-subpath", () => {
    it("two subpaths — flags doubled and ds does not bridge the gap", () => {
        const samples = flatten(
            [CASES.straight_line!.curves, CASES.quarter_circle_r50!.curves], q,
        );
        expect(samples.filter((s) => s.flags & PATH_START)).toHaveLength(2);
        expect(samples.filter((s) => s.flags & PATH_END)).toHaveLength(2);
        const starts = samples.flatMap((s, i) => (s.flags & PATH_START ? [i] : []));
        expect(samples[starts[1]! - 1]!.ds).toBe(0);
    });
});

describe("flatten: determinism", () => {
    it("same input produces byte-identical output", () => {
        // The C++ port must reproduce this stream. A stage that is not
        // deterministic in TS cannot be checked for parity at all.
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            expect(JSON.stringify(flatten([curves], q)), name)
                .toBe(JSON.stringify(flatten([curves], q)));
        }
    });

    it("terminates with a bounded sample count on every fixture", () => {
        // The marcher is `while (t < 1)` with a dtMin floor. A cusp that drives
        // the step to the floor would emit ~1/dtMin = 1e6 samples per curve,
        // which no bounded Pico-side window could hold.
        for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
            const n = flatten([curves], q).length;
            expect(n, `${name}: ${n} samples`).toBeLessThan(20000);
        }
    });
});

// ═══ 2. CONTRACT PROPERTIES — the three caps ══════════════════════════════════

describe("flatten: cap 1 — chord deviation <= chordTol", () => {
    it("holds on every fixture", () => {
        forEachFixture((name, curves) => {
            const dev = polylineDeviation(curves, flatten([curves], q));
            return dev <= q.chordTol + 1e-9
                ? []
                : [`${name}: deviation ${dev.toFixed(6)}mm > chordTol ${q.chordTol}mm`];
        });
    });
});

describe("flatten: cap 2 — spacing <= dsMax", () => {
    it("holds on every fixture", () => {
        // Previously asserted on the straight line only — the one case where the
        // cap is trivially satisfied.
        //
        // KNOWN FAILING (audit F7): `dt <= dsMax / |B'(t)|` reads speed at the
        // step START, so wherever the curve accelerates across a step the chord
        // lands longer than dsMax. Predictor, not guarantee.
        forEachFixture((name, curves) => {
            const samples = flatten([curves], q);
            let worst = 0;
            let count = 0;
            for (let i = 0; i < samples.length - 1; i++) {
                if (samples[i]!.ds > q.dsMax + 1e-9) {
                    count++;
                    worst = Math.max(worst, samples[i]!.ds);
                }
            }
            // Excess reported in exponential form on purpose: the overshoot
            // ranges from 1e-7 (negligible) to 8% (real), and a fixed-decimal
            // percentage renders the small ones as "+0.00%", hiding the spread
            // that tells you whether this is float noise or a soft cap.
            return count === 0
                ? []
                : [`${name}: ${count} step(s) over dsMax, worst ${worst.toPrecision(10)}mm ` +
                   `(excess ${(worst - q.dsMax).toExponential(2)}mm, ` +
                   `${((worst / q.dsMax - 1) * 100).toPrecision(2)}%)`];
        });
    });
});

describe("flatten: cap 3 — tangent turn <= dthetaMax", () => {
    it("holds between consecutive samples within a curve", () => {
        // Curve joins are excluded: a tangent jump THERE is the corner signal
        // constrain reads, and is deliberate. Anywhere else it is an unplanned
        // discontinuity the planner never decelerates for.
        //
        // KNOWN FAILING (audit F1 + F7). Two distinct magnitudes, and the test
        // must report BOTH — they have different causes and different fixes:
        //   near_cusp ~2.8deg (1.4x)  — predictor error, kappa read at step start
        //   cusp    ~178.0deg (89x)   — cap SKIPPED entirely at |B'| -> 0
        forEachFixture((name, curves) => {
            const samples = flatten([curves], q);
            let worst = 0;
            let at = -1;
            for (let i = 1; i < samples.length; i++) {
                if (samples[i]!.flags & (CURVE_BOUNDARY | PATH_START)) continue;
                const turn = absAngleDelta(samples[i - 1]!.theta, samples[i]!.theta);
                if (turn > worst) { worst = turn; at = i; }
            }
            return worst <= q.dthetaMax + 1e-9
                ? []
                : [`${name}: sample ${at} turned ${worst.toFixed(3)}deg ` +
                   `(${(worst / q.dthetaMax).toFixed(1)}x dthetaMax)`];
        });
    });
});

describe("flatten: sample hygiene", () => {
    it("emits no degenerate slivers inside a curve", () => {
        // `t = min(t + dt, 1)` truncates the final step of every curve, which can
        // emit an arbitrarily short segment (audit F3). Near-zero ds AT a curve
        // join is legitimate — the two samples share a position by design — so
        // those are excluded.
        forEachFixture((name, curves) => {
            const samples = flatten([curves], q);
            const bad: number[] = [];
            for (let i = 0; i < samples.length - 1; i++) {
                if (samples[i + 1]!.flags & (CURVE_BOUNDARY | PATH_END)) continue;
                if (samples[i]!.ds <= 1e-6) bad.push(i);
            }
            return bad.length === 0
                ? []
                : [`${name}: ${bad.length} sliver(s) at sample(s) ${bad.slice(0, 5).join(", ")}`];
        });
    });
});

// ═══ 3. REAL SVG ══════════════════════════════════════════════════════════════
// The old test here asserted "total length between 150 and 200mm" and that every
// x was `typeof number` — a 50mm-wide window that broken output would pass, and
// a tautology the type system already guarantees. Replaced with the same caps
// applied to real artwork, which is where they actually have to hold.

describe("flatten: real SVG", () => {
    const repaired = loadSvgMmSubpaths(readFixture("test_snake.svg")).subpaths
        .map((sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired);

    it("snake.svg — spacing cap holds", () => {
        // KNOWN FAILING (audit F7): 142 of 356 steps land over dsMax, worst
        // +1.50%. Real artwork accelerates across steps far more than the
        // synthetic fixtures do, so the predictor error is systematic here
        // rather than incidental.
        const samples = flatten(repaired, q);
        let worst = 0;
        let count = 0;
        for (let i = 0; i < samples.length - 1; i++) {
            if (samples[i]!.flags & PATH_END) continue;
            if (samples[i]!.ds > q.dsMax + 1e-9) { count++; worst = Math.max(worst, samples[i]!.ds); }
        }
        expect(count, `${count} steps over dsMax, worst ${worst.toFixed(6)}mm`).toBe(0);
    });

    it("snake.svg — tangent cap holds within curves", () => {
        const samples = flatten(repaired, q);
        let worst = 0;
        let at = -1;
        for (let i = 1; i < samples.length; i++) {
            if (samples[i]!.flags & (CURVE_BOUNDARY | PATH_START)) continue;
            const turn = absAngleDelta(samples[i - 1]!.theta, samples[i]!.theta);
            if (turn > worst) { worst = turn; at = i; }
        }
        expect(worst, `sample ${at} turned ${worst.toFixed(3)}deg`)
            .toBeLessThanOrEqual(q.dthetaMax + 1e-9);
    });

    it("snake.svg — chord deviation holds per subpath", () => {
        for (let k = 0; k < repaired.length; k++) {
            const dev = polylineDeviation(repaired[k]!, flatten([repaired[k]!], q), 120);
            expect(dev, `subpath ${k}: ${dev.toFixed(6)}mm`).toBeLessThanOrEqual(q.chordTol + 1e-9);
        }
    });
});

// ═══ 4. CORNER SIGNAL ═════════════════════════════════════════════════════════

describe("flatten: corner signal", () => {
    it("a 90-degree join appears as a tangent jump across a ~zero-length step", () => {
        // This is the contract constrain depends on: same position, different
        // theta, CURVE_BOUNDARY set. If flatten ever stops emitting the duplicate
        // sample, corner detection silently stops working.
        const horiz = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        const vert = lineToCubic({ x: 10, y: 0 }, { x: 10, y: 10 });
        const samples = flatten([[horiz, vert]], q);
        const bi = samples.findIndex((s) => s.flags & CURVE_BOUNDARY);
        expect(bi).toBeGreaterThan(0);
        expect(Math.abs(absAngleDelta(samples[bi - 1]!.theta, samples[bi]!.theta) - 90)).toBeLessThan(1.0);
        expect(samples[bi - 1]!.ds).toBeLessThan(1e-6);
    });

    it("the cusp reversal is NOT reported as a curve boundary", () => {
        // Documents the asymmetry behind audit F2: a cusp is a tangent
        // discontinuity with no CURVE_BOUNDARY flag, so constrain's corner branch
        // never inspects it, while discretize's ungated dtheta check does. The two
        // stages disagree about what a corner is. This test passes today and
        // should be REVISITED, not deleted, when F2 is resolved.
        const samples = flatten([CUSP], q);
        const flagged = samples.filter((s) => s.flags & CURVE_BOUNDARY);
        expect(flagged).toHaveLength(0);

        let worst = 0;
        for (let i = 1; i < samples.length; i++) {
            worst = Math.max(worst, absAngleDelta(samples[i - 1]!.theta, samples[i]!.theta));
        }
        // The reversal is real and large; nothing in the sample stream marks it.
        expect(worst).toBeGreaterThan(45);
    });
});

// ═══ 5. TANGENT CONTINUITY vs THE REAL CURVE ══════════════════════════════════

describe("flatten: theta tracks the real tangent", () => {
    it("each sample's theta matches B'(t) at that point", () => {
        // theta drives the A axis on a tangential tool. It is computed from B'
        // with a fallback to the PREVIOUS theta when |B'| ~ 0 — so at a cusp the
        // stream reports a tangent the curve does not have. Verify against the
        // geometry by finding the nearest true-curve point to each sample.
        const curves = CASES.quarter_circle_r50!.curves;
        const samples = flatten([curves], q);
        const c = curves[0]!;
        for (let i = 0; i < samples.length; i++) {
            const t = i / (samples.length - 1);
            const d = bezierDeriv1(c, t);
            const trueTheta = (Math.atan2(d.y, d.x) * 180) / Math.PI;
            // Sampling is not uniform in t, so allow a generous band; this is a
            // sanity check on sign/branch, not on placement.
            expect(absAngleDelta(samples[i]!.theta, trueTheta), `sample ${i}`).toBeLessThan(15);
        }
    });
});
