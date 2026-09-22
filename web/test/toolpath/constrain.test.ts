/**
 * Tests for the Constrain stage (redesign stage 5): per-sample velocity ceiling.
 *
 * Same split as flatten.test.ts:
 *
 *   1. INVARIANTS — true of any correct constrain. Bounded output, purity,
 *      determinism. These must survive the C++ port unchanged.
 *
 *   2. CONTRACT PROPERTIES — the caps that define the stage:
 *        vCeiling <= feedMax
 *        vCeiling <= sqrt(aMax / kappa)              centripetal
 *        vCeiling <= rad(aRateDegS) / kappa          A slew
 *        vCeiling <= sqrt(rad(aAccelDegS2) / |k'|)   A angular accel
 *        vCeiling == 0 at a corner-stop or a forcedStop
 *      Asserted over every fixture rather than at one hand-picked sample.
 *
 * Where a cap's formula would have to be re-derived in the test to check it
 * directly (the curvature-gradient term needs `kappaPrime`, which is internal),
 * MONOTONICITY is asserted instead: tightening any limit must never raise any
 * ceiling. That is implementation-independent, catches a botched min() chain —
 * the single most likely transcription error in the port — and needs none of
 * constrain's internals.
 *
 * The test is the caller: it sources config values inline and passes them via
 * ConstrainOptions. constrain() never imports config.
 *
 * See docs/planner_audit.md for findings.
 */

import { describe, it, expect } from "vitest";
import { lineToCubic, angleDelta, type CubicBezier } from "../../src/toolpath/geometry.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { constrain, junctionCap, type ConstrainOptions } from "../../src/toolpath/constrain.js";
import { CURVE_BOUNDARY, PATH_START, PATH_END, type Sample } from "../../src/toolpath/sample.js";
import { CASES, CUSP } from "./curves.cases.js";
import { qualityConfig } from "../../src/machine/index.js";
import { readFixture } from "../helpers.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";
import { enforceC1 } from "../../src/toolpath/repair.js";

const FEED = 80.0;
const A_MAX = 1000.0;
const q = qualityConfig();

const BASE: ConstrainOptions = {
    feedMax: FEED,
    aMax: A_MAX,
    junctionDeviation: q.junctionDeviation,
};

/** Every fixture, including the cusp that lives outside the CASES registry. */
const GEOMETRY_CASES: Readonly<Record<string, CubicBezier[]>> = {
    ...Object.fromEntries(Object.entries(CASES).map(([k, v]) => [k, v.curves])),
    cusp: CUSP,
};

const samplesFor = (curves: CubicBezier[]): Sample[] => flatten([curves], q);

/**
 * Run `probe` over every fixture and fail ONCE with all violations.
 * See flatten.test.ts — an `expect` inside a fixture loop reports only the first
 * failure and silently skips the rest, which is how the worst case hides.
 */
function forEachFixture(probe: (name: string, samples: Sample[]) => string[]): void {
    const problems: string[] = [];
    for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
        problems.push(...probe(name, samplesFor(curves)));
    }
    if (problems.length === 0) return;
    const shown = problems.slice(0, 12).join("\n  ");
    const rest = problems.length > 12 ? `\n  ...and ${problems.length - 12} more` : "";
    throw new Error(`${problems.length} violation(s):\n  ${shown}${rest}`);
}

// ═══ 1. INVARIANTS ════════════════════════════════════════════════════════════

describe("constrain: bounds", () => {
    it("vCeiling never exceeds feedMax", () => {
        forEachFixture((name, s) => {
            const c = constrain(s, { ...BASE, aRateDegS: 100, cornerStopAngleDeg: 20 });
            const over = c.filter((x) => x.vCeiling > FEED + 1e-6).length;
            return over === 0 ? [] : [`${name}: ${over} sample(s) above feedMax`];
        });
    });

    it("vCeiling is finite and non-negative", () => {
        // kappa can be enormous at a cusp and kappaPrime larger still; every cap
        // is a division by one of them. A NaN or negative here would propagate
        // into plan's sqrt and out the far end as a garbage interval.
        forEachFixture((name, s) => {
            const c = constrain(s, { ...BASE, aRateDegS: 100, aAccelDegS2: 50, cornerStopAngleDeg: 20 });
            const bad = c.flatMap((x, i) =>
                Number.isFinite(x.vCeiling) && x.vCeiling >= 0 ? [] : [`${i}=${x.vCeiling}`]);
            return bad.length === 0 ? [] : [`${name}: ${bad.slice(0, 4).join(", ")}`];
        });
    });

    it("does not mutate its input", () => {
        // constrain documents itself as pure. The port will reuse buffers for
        // memory reasons, which is exactly when accidental mutation appears.
        const s = samplesFor(CASES.s_curve!.curves);
        const before = JSON.stringify(s);
        constrain(s, { ...BASE, aRateDegS: 100, aAccelDegS2: 50, cornerStopAngleDeg: 20 });
        expect(JSON.stringify(s)).toBe(before);
    });

    it("is deterministic", () => {
        forEachFixture((name, s) => {
            const a = JSON.stringify(constrain(s, { ...BASE, aRateDegS: 100 }));
            const b = JSON.stringify(constrain(s, { ...BASE, aRateDegS: 100 }));
            return a === b ? [] : [`${name}: two runs differ`];
        });
    });

    it("preserves sample count and geometry", () => {
        // constrain adds a field; it must not drop, reorder or edit samples.
        forEachFixture((name, s) => {
            const c = constrain(s, BASE);
            if (c.length !== s.length) return [`${name}: length ${c.length} != ${s.length}`];
            for (let i = 0; i < s.length; i++) {
                const a = s[i]!;
                const b = c[i]!;
                if (a.x !== b.x || a.y !== b.y || a.kappa !== b.kappa || a.ds !== b.ds || a.flags !== b.flags) {
                    return [`${name}: sample ${i} altered`];
                }
            }
            return [];
        });
    });
});

// ═══ 2. CONTRACT PROPERTIES ═══════════════════════════════════════════════════

describe("constrain: straight line", () => {
    it("ceiling == feedMax everywhere", () => {
        for (const x of constrain(samplesFor(CASES.straight_line!.curves), BASE)) {
            expect(Math.abs(x.vCeiling - FEED)).toBeLessThan(1e-6);
        }
    });
});

describe("constrain: centripetal cap", () => {
    it("holds at EVERY sample on every fixture", () => {
        // Was checked at one hand-picked mid-sample on one fixture. The cap is a
        // per-sample contract, so assert it per sample.
        forEachFixture((name, s) => {
            const c = constrain(s, BASE);
            let worst = 0;
            let count = 0;
            for (let i = 0; i < s.length; i++) {
                if (s[i]!.kappa <= 1e-9) continue;
                const lim = Math.sqrt(A_MAX / s[i]!.kappa);
                if (c[i]!.vCeiling > lim + 1e-9) { count++; worst = Math.max(worst, c[i]!.vCeiling / lim); }
            }
            return count === 0 ? [] : [`${name}: ${count} sample(s), worst ${worst.toFixed(3)}x the cap`];
        });
    });

    it("r5 circle: kappa=0.2 -> v ~ sqrt(1000/0.2) ~ 70.7", () => {
        const c = constrain(samplesFor(CASES.quarter_circle_r5!.curves), BASE);
        const expected = Math.sqrt(A_MAX / 0.2);
        const mid = c[Math.floor(c.length / 2)]!.vCeiling;
        expect(Math.abs(mid - expected) / expected).toBeLessThan(0.05);
    });

    it("scales as sqrt(aMax)", () => {
        // Pins the sqrt. A cap written as aMax/kappa (the A-slew form, easy to
        // paste into the wrong branch) would scale linearly and give 4x here.
        const mid = (aMax: number) => {
            const c = constrain(samplesFor(CASES.quarter_circle_r5!.curves), { ...BASE, aMax });
            return c[Math.floor(c.length / 2)]!.vCeiling;
        };
        expect(mid(1000) / mid(250)).toBeCloseTo(2.0, 6);
    });

    it("tighter circle -> lower cap", () => {
        const mid = (curves: CubicBezier[]) => {
            const c = constrain(samplesFor(curves), BASE);
            return c[Math.floor(c.length / 2)]!.vCeiling;
        };
        expect(mid(CASES.quarter_circle_r5!.curves)).toBeLessThan(mid(CASES.quarter_circle_r50!.curves));
    });
});

describe("constrain: A-slew cap", () => {
    it("holds at EVERY sample on every fixture", () => {
        const aRateDegS = 100.0;
        const aRateRad = (aRateDegS * Math.PI) / 180;
        forEachFixture((name, s) => {
            const c = constrain(s, { ...BASE, aRateDegS });
            let worst = 0;
            let count = 0;
            for (let i = 0; i < s.length; i++) {
                if (s[i]!.kappa <= 1e-9) continue;
                const lim = aRateRad / s[i]!.kappa;
                if (c[i]!.vCeiling > lim + 1e-9) { count++; worst = Math.max(worst, c[i]!.vCeiling / lim); }
            }
            return count === 0 ? [] : [`${name}: ${count} sample(s), worst ${worst.toFixed(3)}x the cap`];
        });
    });

    it("slow A axis lowers the ceiling on a tight curve", () => {
        const mid = (o: ConstrainOptions) => {
            const c = constrain(samplesFor(CASES.quarter_circle_r5!.curves), o);
            return c[Math.floor(c.length / 2)]!.vCeiling;
        };
        const withA = mid({ ...BASE, aRateDegS: 100.0 });
        expect(withA).toBeLessThan(mid(BASE));
        expect(Math.abs(withA - (Math.PI * 100 / 180) / 0.2) / withA).toBeLessThan(0.05);
    });
});

describe("constrain: A-accel gradient cap", () => {
    it("changing curvature — tight a_accel lowers the min ceiling", () => {
        const minOf = (o: ConstrainOptions) =>
            constrain(samplesFor(CASES.s_curve!.curves), o).reduce((m, x) => Math.min(m, x.vCeiling), Infinity);
        expect(minOf({ ...BASE, aAccelDegS2: 50.0 })).toBeLessThan(minOf(BASE));
    });

    it("constant curvature — cap inactive (dk/ds = 0)", () => {
        const at = (o: ConstrainOptions) => {
            const c = constrain(samplesFor(CASES.quarter_circle_r50!.curves), o);
            return c[Math.floor(c.length / 2)]!.vCeiling;
        };
        expect(Math.abs(at(BASE) - at({ ...BASE, aAccelDegS2: 50.0 }))).toBeLessThan(1e-9);
    });

    // ── FINDING C3 ────────────────────────────────────────────────────────────
    // The two tests above are both blind to a cap that is too TIGHT, and that is
    // structural rather than incidental: monotonicity is one-sided, and the
    // scaling law is a ratio, so any constant factor on |k'| cancels out of it.
    // Two kappaPrime mutants survive every other test in this file — halving the
    // arc-length span (slows every curvature-varying move by sqrt(2)) and
    // dropping the guard that stops a difference straddling a curve join (a
    // near-zero ceiling at every curve join in every job). Found by mutating the
    // C++ port; see docs/planner_audit.md C3. Both tests below are two-sided.

    it("binds at exactly sqrt(alpha/|k'|) where it is the active cap", () => {
        // |k'| is re-derived here by the same central difference constrain uses,
        // so this cannot catch the two agreeing on a wrong definition of dk/ds —
        // it catches a wrong span, a wrong index, or a missing guard.
        const aAccelDegS2 = 50.0;
        const aAccRad = (aAccelDegS2 * Math.PI) / 180;
        const s = samplesFor(CASES.s_curve!.curves);
        const c = constrain(s, { ...BASE, aAccelDegS2 });
        const BREAK = PATH_START | PATH_END | CURVE_BOUNDARY;

        let binding = 0;
        const bad: string[] = [];
        for (let i = 1; i < s.length - 1; i++) {
            if (s[i]!.flags & BREAK || s[i + 1]!.flags & BREAK) continue;
            const span = s[i - 1]!.ds + s[i]!.ds;
            if (span < 1e-6) continue;
            const kp = Math.abs(s[i + 1]!.kappa - s[i - 1]!.kappa) / span;
            if (kp <= 1e-9) continue;

            const lim = Math.sqrt(aAccRad / kp);
            let others = FEED;
            if (s[i]!.kappa > 1e-9) others = Math.min(others, Math.sqrt(A_MAX / s[i]!.kappa));
            if (lim >= others * 0.999) continue; // only where this cap is the min

            binding++;
            if (Math.abs(c[i]!.vCeiling - lim) > 1e-9 && bad.length < 4) {
                bad.push(`${i}: got ${c[i]!.vCeiling} expected ${lim}`);
            }
        }
        // Guards against the test going vacuous if s_curve stops driving the cap.
        expect(binding, "cap never bound").toBeGreaterThan(10);
        expect(bad.join("; ")).toBe("");
    });

    it("a kappa jump ACROSS a curve join is not an angular acceleration", () => {
        // kappa is genuinely discontinuous where two curves meet; the jump is an
        // artefact of the representation, not a rotation the A axis performs.
        // Stated behaviourally: enabling the cap must change NOTHING there.
        const BREAK = PATH_START | PATH_END | CURVE_BOUNDARY;
        let checked = 0;
        forEachFixture((name, s) => {
            const a = constrain(s, BASE);
            const b = constrain(s, { ...BASE, aAccelDegS2: 50.0 });
            const bad: string[] = [];
            for (let i = 1; i < s.length - 1; i++) {
                if (!(s[i]!.flags & BREAK || s[i + 1]!.flags & BREAK)) continue;
                checked++;
                if (a[i]!.vCeiling !== b[i]!.vCeiling && bad.length < 4) {
                    bad.push(`${name}[${i}]: ${a[i]!.vCeiling} -> ${b[i]!.vCeiling}`);
                }
            }
            return bad;
        });
        expect(checked, "no boundary samples examined").toBeGreaterThan(5);
    });

    it("scales as sqrt(aAccel) where the cap binds", () => {
        // The only assertion that pins the FORM of this cap rather than its
        // direction. v <= sqrt(alpha/|k'|), so 4x the angular-accel budget must
        // buy exactly 2x the speed. A cap implemented as alpha/|k'| — a plausible
        // transcription slip, and dimensionally wrong — would give 4x and fail.
        const minOf = (aAccelDegS2: number) =>
            constrain(samplesFor(CASES.s_curve!.curves), { ...BASE, aAccelDegS2 })
                .reduce((m, x) => Math.min(m, x.vCeiling), Infinity);
        const lo = minOf(12.5);
        const hi = minOf(50.0);
        expect(hi / lo).toBeCloseTo(2.0, 3);
    });
});

// ── monotonicity: the min() chain, checked without re-deriving it ─────────────

describe("constrain: monotonicity", () => {
    /** Tightening a limit must never raise ANY ceiling. */
    function assertNeverRaises(label: string, loose: ConstrainOptions, tight: ConstrainOptions): void {
        forEachFixture((name, s) => {
            const a = constrain(s, loose);
            const b = constrain(s, tight);
            const raised = b.flatMap((x, i) =>
                x.vCeiling > a[i]!.vCeiling + 1e-9
                    ? [`${i}: ${a[i]!.vCeiling.toFixed(4)} -> ${x.vCeiling.toFixed(4)}`]
                    : []);
            return raised.length === 0
                ? []
                : [`${label} on ${name}: ${raised.length} ceiling(s) went UP, e.g. ${raised[0]}`];
        });
    }

    it("lowering feedMax never raises a ceiling", () => {
        assertNeverRaises("feedMax 80->40", BASE, { ...BASE, feedMax: 40 });
    });

    it("lowering aMax never raises a ceiling", () => {
        assertNeverRaises("aMax 1000->250", BASE, { ...BASE, aMax: 250 });
    });

    it("enabling the A-slew cap never raises a ceiling", () => {
        assertNeverRaises("aRateDegS off->100", BASE, { ...BASE, aRateDegS: 100 });
    });

    it("enabling the A-accel cap never raises a ceiling", () => {
        assertNeverRaises("aAccelDegS2 off->50", BASE, { ...BASE, aAccelDegS2: 50 });
    });

    it("lowering the corner-stop threshold never raises a ceiling", () => {
        assertNeverRaises(
            "cornerStop 90->20",
            { ...BASE, cornerStopAngleDeg: 90 },
            { ...BASE, cornerStopAngleDeg: 20 },
        );
    });
});

// ── corner stop ───────────────────────────────────────────────────────────────

describe("constrain: corner stop", () => {
    const rightAngle = () => flatten([[
        lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 }),
        lineToCubic({ x: 10, y: 0 }, { x: 10, y: 10 }),
    ]], q);

    it("sharp corner forces vCeiling = 0", () => {
        const s = rightAngle();
        const c = constrain(s, { ...BASE, cornerStopAngleDeg: 20.0 });
        expect(c[c.findIndex((x) => x.flags & CURVE_BOUNDARY)]!.vCeiling).toBe(0);
    });

    it("no corner stop when disabled — junction cap still applies", () => {
        const c = constrain(rightAngle(), BASE);
        const bi = c.findIndex((x) => x.flags & CURVE_BOUNDARY);
        expect(c[bi]!.vCeiling).toBeGreaterThan(0);
        expect(c[bi]!.vCeiling).toBeLessThan(FEED);
    });

    it("a corner stop lands on the boundary sample only", () => {
        // discretize choreographs the lift-pivot-lower around this exact index.
        // If the zero ever spreads to a neighbour, the pivot is placed wrong.
        const c = constrain(rightAngle(), { ...BASE, cornerStopAngleDeg: 20.0 });
        const zeros = c.flatMap((x, i) => (x.vCeiling === 0 ? [i] : []));
        const boundaries = c.flatMap((x, i) => (x.flags & CURVE_BOUNDARY ? [i] : []));
        expect(zeros).toEqual(boundaries);
    });
});

describe("constrain: junctionCap helper", () => {
    it("monotone — sharper turn lowers the cap; straight = feedMax", () => {
        const straight = junctionCap(1.0, A_MAX, 0.05, FEED);
        const gentle = junctionCap(30.0, A_MAX, 0.05, FEED);
        const sharp = junctionCap(120.0, A_MAX, 0.05, FEED);
        expect(straight).toBeGreaterThanOrEqual(gentle);
        expect(gentle).toBeGreaterThanOrEqual(sharp);
        expect(junctionCap(0.0, A_MAX, 0.05, FEED)).toBe(FEED);
    });

    it("a full reversal caps at zero", () => {
        // cos(180/2) = 0 -> the arc radius collapses. The tool cannot carry any
        // speed through a doubling-back join.
        expect(junctionCap(180.0, A_MAX, 0.05, FEED)).toBe(0);
    });

    it("is symmetric in turn direction", () => {
        for (const deg of [15, 45, 90, 150]) {
            expect(junctionCap(-deg, A_MAX, 0.05, FEED)).toBe(junctionCap(deg, A_MAX, 0.05, FEED));
        }
    });

    it("a larger deviation budget allows more speed", () => {
        expect(junctionCap(45, A_MAX, 0.2, FEED)).toBeGreaterThan(junctionCap(45, A_MAX, 0.02, FEED));
    });

    it("matches the closed form at 90 degrees", () => {
        // GRBL junction deviation: model the corner as an arc of radius
        //   r = d*cos(t/2) / (1 - cos(t/2))
        // and hold centripetal accel on it, v = sqrt(a*r). Computed here from
        // the definition rather than copied from the implementation, so a
        // dropped factor shows up as a number rather than as a direction.
        const halfCos = Math.cos(Math.PI / 4);
        const r = (0.05 * halfCos) / (1 - halfCos);
        expect(junctionCap(90, A_MAX, 0.05, FEED)).toBeCloseTo(Math.sqrt(A_MAX * r), 9);
    });

    it("scales as sqrt(deviation) below the feed clamp", () => {
        // v ~ sqrt(a*r) and r ~ deviation, so 4x the budget is 2x the speed.
        // A sharp turn is used so neither result is clamped at feedMax.
        const lo = junctionCap(150, A_MAX, 0.01, FEED);
        const hi = junctionCap(150, A_MAX, 0.04, FEED);
        expect(hi).toBeLessThan(FEED);
        expect(hi / lo).toBeCloseTo(2.0, 6);
    });
});

// ═══ 3. THE CUSP — what constrain actually does ═══════════════════════════════

describe("constrain: cusp handling", () => {
    it("F2 (FIXED): stops at an intra-curve tangent reversal", () => {
        // Was: the corner-stop branch was gated on CURVE_BOUNDARY, which flatten
        // only sets at curve JOINS. A 178deg reversal INSIDE a single curve was
        // never considered for a corner stop — while discretize's ungated
        // dtheta check treated it as one and inserted a lift-pivot-lower there.
        // The two stages disagreed about what a corner is.
        const s = flatten([CUSP], q);
        const c = constrain(s, { ...BASE, cornerStopAngleDeg: 20.0 });
        expect(s.filter((x) => x.flags & CURVE_BOUNDARY)).toHaveLength(0);
        expect(c.filter((x) => x.vCeiling === 0).length).toBeGreaterThan(0);
    });

    it("F2 (FIXED): the stop lands on the sample the pivot happens at", () => {
        // Off-by-one guard, and it is not hypothetical: the finding test for
        // this in discretize.test.ts checked the sample BEFORE the jump while
        // printing the flag of the sample AFTER it, so it under-reported the
        // defect (4.84e-3 mm/s at the approach sample, 2.14e-2 at the sample
        // that actually pivots). The corner is at the LATER sample of the pair.
        const s = flatten([CUSP], q);
        const c = constrain(s, { ...BASE, cornerStopAngleDeg: 20.0 });
        let found = 0;
        for (let i = 1; i < s.length; i++) {
            if (Math.abs(angleDelta(s[i - 1]!.theta, s[i]!.theta)) >= 20.0) {
                found++;
                expect(c[i]!.vCeiling).toBe(0);
            }
        }
        expect(found).toBeGreaterThan(0);
    });

    it("C1 (FIXED): a ceiling below vMin becomes a stop, not a crawl", () => {
        // Was: the A caps drove the ceiling to ~3e-3 mm/s — 166x BELOW
        // quality.vMin (0.5 mm/s), the floor discretize clamps the interval to.
        // The planned and executed profiles diverged by two orders of magnitude
        // there, and every timeline derived from the plan went with them
        // (notably dutyBreaks' budget).
        const c = constrain(flatten([CUSP], q), {
            ...BASE, aRateDegS: 100, aAccelDegS2: 50, vMin: q.vMin,
        });
        const nonZero = c.map((x) => x.vCeiling).filter((v) => v > 0);
        expect(Math.min(...nonZero)).toBeGreaterThanOrEqual(q.vMin);
        expect(c.some((x) => x.vCeiling === 0)).toBe(true);
    });

    it("C1: the floor is opt-in — absent vMin leaves the old crawl", () => {
        // The stage takes no config and invents no defaults: a caller that does
        // not state a floor does not get one. This is what keeps constrain
        // usable outside the production bridge, and it is why the value is
        // passed rather than imported.
        const c = constrain(flatten([CUSP], q), { ...BASE, aRateDegS: 100, aAccelDegS2: 50 });
        const min = Math.min(...c.map((x) => x.vCeiling).filter((v) => v > 0));
        expect(min).toBeLessThan(q.vMin / 100);
    });
});

// ═══ 4. REAL SVG ══════════════════════════════════════════════════════════════

describe("constrain: real SVG", () => {
    const repaired = loadSvgMmSubpaths(readFixture("test_snake.svg")).subpaths
        .map((sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired);

    it("snake.svg — every cap holds and the output is usable", () => {
        const s = flatten(repaired, q);
        const c = constrain(s, { ...BASE, aRateDegS: 100, aAccelDegS2: 50, cornerStopAngleDeg: 20 });
        const aRateRad = (100 * Math.PI) / 180;
        const bad: string[] = [];
        for (let i = 0; i < s.length; i++) {
            const v = c[i]!.vCeiling;
            if (!Number.isFinite(v) || v < 0 || v > FEED + 1e-6) bad.push(`${i}: v=${v}`);
            if (s[i]!.kappa > 1e-9) {
                if (v > Math.sqrt(A_MAX / s[i]!.kappa) + 1e-9) bad.push(`${i}: centripetal`);
                if (v > aRateRad / s[i]!.kappa + 1e-9) bad.push(`${i}: A-slew`);
            }
        }
        expect(bad.slice(0, 6).join("; ")).toBe("");
    });

    it("snake.svg — only boundary samples are forced to zero", () => {
        const s = flatten(repaired, q);
        const c = constrain(s, { ...BASE, cornerStopAngleDeg: 20 });
        for (const [i, x] of c.entries()) {
            if (x.vCeiling !== 0) continue;
            expect(s[i]!.flags & (CURVE_BOUNDARY | PATH_START), `sample ${i} zeroed`).toBeTruthy();
        }
    });
});

// ═══ 5. FORCED STOPS ══════════════════════════════════════════════════════════
// A stop the CALLER injects, for a reason the geometry knows nothing about —
// today, releasing a duty-limited tool's enable line before its budget expires
// (docs/tool_duty_limits.md §5 tier 2).

describe("constrain: forcedStops", () => {
    const straight = () => flatten([[lineToCubic({ x: 0, y: 0 }, { x: 100, y: 0 })]], q);
    const opts = (forcedStops?: ReadonlySet<number>): ConstrainOptions => ({ ...BASE, forcedStops });

    it("zeroes the ceiling at the named sample and nowhere else", () => {
        const s = straight();
        const idx = Math.floor(s.length / 2);
        const c = constrain(s, opts(new Set([idx])));
        expect(c[idx]!.vCeiling).toBe(0);
        for (let i = 0; i < c.length; i++) {
            if (i !== idx) expect(c[i]!.vCeiling).toBeGreaterThan(0);
        }
    });

    it("is a no-op when absent or empty — the byte-for-byte guarantee", () => {
        // Every existing caller passes nothing. If this ever diverges, the
        // golden snapshot moves for every tool, duty-limited or not.
        const s = straight();
        const base = constrain(s, opts());
        for (const alt of [constrain(s, opts(new Set())), constrain(s, opts(undefined))]) {
            expect(alt.map((x) => x.vCeiling)).toEqual(base.map((x) => x.vCeiling));
        }
    });

    it("beats every geometric cap, including a straight line at full feed", () => {
        // On a straight run nothing else constrains the sample, so a min()
        // against feedMax would leave it at 80 mm/s. This is the case that
        // proves the override is an override.
        const s = straight();
        const idx = Math.floor(s.length / 2);
        expect(constrain(s, opts())[idx]!.vCeiling).toBeCloseTo(FEED, 6);
        expect(constrain(s, opts(new Set([idx])))[idx]!.vCeiling).toBe(0);
    });

    it("accepts several stops at once", () => {
        const s = straight();
        const stops = new Set([2, 5, 9]);
        const c = constrain(s, opts(stops));
        for (const i of stops) expect(c[i]!.vCeiling).toBe(0);
    });

    it("ignores out-of-range indices rather than throwing", () => {
        // The caller measured a timeline from a PREVIOUS bake; a stale index is
        // a scheduling bug, not a crash. Failing soft keeps the pipeline's
        // error surface at the stage that can explain it.
        const s = straight();
        const c = constrain(s, opts(new Set([-1, s.length, 9999])));
        expect(c.map((x) => x.vCeiling)).toEqual(constrain(s, opts()).map((x) => x.vCeiling));
    });

    it("wins over a corner stop at the same index", () => {
        // Both produce 0, so this pins intent rather than arithmetic: the early
        // return must not be reordered below the corner branch during the port.
        const s = flatten([[
            lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 }),
            lineToCubic({ x: 10, y: 0 }, { x: 10, y: 10 }),
        ]], q);
        const bi = s.findIndex((x) => x.flags & CURVE_BOUNDARY);
        const c = constrain(s, { ...BASE, cornerStopAngleDeg: 20, forcedStops: new Set([bi]) });
        expect(c[bi]!.vCeiling).toBe(0);
    });
});
