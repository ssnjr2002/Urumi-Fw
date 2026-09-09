/**
 * Constrain stage (redesign stage 5): assign each Sample a LOCAL velocity ceiling.
 * Ported from pipeline/stages/constrain.py.
 *
 * Pure, per-sample, no propagation — "how fast could the tool ever go right
 * here?" The forward/backward feasibility sweeps that turn these ceilings into
 * a reachable profile are the Plan stage (plan_lookahead.ts, future).
 *
 * Ceiling at sample i = min of:
 *   feedMax                         programmed cruise limit
 *   sqrt(aMax / kappa_i)            centripetal (XY radial accel) — LOCAL kappa
 *   rad(aRateDegS) / kappa_i        A-axis slew: a tangential tool rotates at
 *                                   dθ/dt = kappa·v, so cap v to the A slew ceiling
 *   sqrt(rad(aAccelDegS2) / |κ'|)   A angular-accel, curvature-gradient term
 *   junction-deviation cap          at a curve-boundary tangent jump below the
 *                                   corner threshold (GRBL-style cornering)
 *   0                               at a curve-boundary jump >= corner threshold
 *                                   (a lift-pivot corner — the tool must stop)
 *
 * Using LOCAL kappa per sample is the whole point: a degenerate curvature spike
 * caps ONE sample, not a whole curve.
 *
 * Pure stage: takes ConstrainOptions (a focused subset spanning three config
 * tiers), never imports config. The caller bridges config to stage at the
 * call site.
 */

import {
    CURVE_BOUNDARY,
    PATH_START,
    PATH_END,
    type Sample,
} from "./sample.js";
import { angleDelta } from "./geometry.js";

// κ-discontinuity flags: a finite difference of curvature must not straddle a
// curve/subpath boundary (κ is discontinuous there).
const KAPPA_BREAK = PATH_START | PATH_END | CURVE_BOUNDARY;

// ── types ─────────────────────────────────────────────────────────────────────

/**
 * Constrain stage output: a Sample with a local velocity ceiling filled in.
 *
 * The type progression begins here — flatten emits Sample[], constrain emits
 * ConstrainedSample[], plan (future) will emit PlannedSample. Each stage's
 * output type documents its guarantee; the compiler catches skipped stages.
 */
export interface ConstrainedSample extends Sample {
    readonly vCeiling: number;
}

/**
 * Constrain stage options.
 *
 * Six parameters spanning three config tiers — useful to know when
 * constructing overrides at the call site:
 *
 *   feed/accel     -> feedMax (resolved pathFeed), aRateDegS (A maxFeed),
 *                     aAccelDegS2 (A maxAccel)
 *   MachineConfig  -> aMax (min X/Y maxAccel)
 *   ToolProfile    -> cornerStopAngleDeg
 *   QualityConfig  -> junctionDeviation
 *
 * The three "always needed" values are required. The optional ones are
 * "disable switches" — their absence naturally means "disabled" (no A cap, no
 * forced corner stops, no injected stops). No config import, no hidden
 * defaults; the caller bridges all three tiers.
 *
 * `forcedStops` is the odd one out: it comes from no config tier at all, but
 * from a downstream stage that measured a baked timeline and needs a stop the
 * geometry does not imply. See its own doc comment.
 */
export interface ConstrainOptions {
    // Required — always needed
    /** Programmed cruise ceiling (mm/s). Source: resolved pathFeed (tool.path ?? machine.path). */
    readonly feedMax: number;
    /**
     * Lateral acceleration for the centripetal cap (mm/s²). Source: min(x.maxAccel, y.maxAccel).
     * NOTE: this is a single scalar — a square-machine assumption. On a non-square machine
     * (x.maxAccel ≠ y.maxAccel) the correct value is Math.min(x.maxAccel, y.maxAccel).
     */
    readonly aMax: number;
    /** Corner-rounding budget (mm). Source: QualityConfig.junctionDeviation. */
    readonly junctionDeviation: number;

    // Optional — absence = disabled
    /** Tangential tool A-slew (velocity) ceiling (deg/s); 0/undefined disables. Source: AxisConfig.maxFeed (A). */
    readonly aRateDegS?: number;
    /** Tangential tool A angular-acceleration ceiling (deg/s²); 0/undefined disables. Source: AxisConfig.maxAccel (A). */
    readonly aAccelDegS2?: number;
    /** Boundary tangent jump (deg) at/above which vCeiling is forced to 0 (lift-pivot). Source: ToolProfile.cornerAngleDeg. */
    readonly cornerStopAngleDeg?: number;
    /**
     * Sample indices at which vCeiling is forced to 0 regardless of geometry —
     * a stop the CALLER needs for a reason the geometry knows nothing about.
     *
     * The motivating case is a duty-limited tool (docs/tool_duty_limits.md §5
     * tier 2): the ultrasonic knife must have its enable line released before
     * its budget expires, and if the toolpath offers no natural lift inside the
     * band, one has to be created. A stop here is what makes that possible —
     * plan's backward sweep decelerates into it and the forward sweep
     * accelerates out, so discretize can lift at a sample that is genuinely at
     * rest rather than one merely marked as such.
     *
     * Indices are into `samples` as passed. Out-of-range entries are ignored;
     * this is a hint from a stage that measured a timeline, not a contract that
     * can be checked here.
     */
    readonly forcedStops?: ReadonlySet<number>;
    /**
     * Execution speed floor (mm/s). A ceiling below this is not a ceiling — it
     * is a stop that has not admitted it, so it is forced to 0 and the corner
     * machinery handles it honestly.
     *
     * Source: QualityConfig.vMin — the SAME value `discretize` clamps the step
     * interval to. Passed in explicitly rather than imported, because this
     * stage takes no config and that property is worth keeping. Absence
     * disables the floor, like the other optional switches.
     *
     * Audit C1: on the cusp fixture the A caps drove the ceiling to 3.24e-3
     * mm/s while discretize executed it at vMin = 0.5 — the plan and the
     * machine disagreed by 166x, and every timeline derived from the plan was
     * wrong across the cusp with it.
     */
    readonly vMin?: number;
    /**
     * Per-sample ceilings measured in PACKET space and fed back — indexed like
     * `samples`, absent/undefined/negative entries ignored.
     *
     * The A-slew ceiling above is exact in the continuum: a tangential tool
     * turning through κ·ds radians over ds mm holds ω = κ·v, so v ≤ ω_max/κ.
     * What discretize emits is not that. It emits an integer `da` over an
     * integer (dx, dy), and `interval()` floors the segment time at
     * |da| / (a.maxFeed · a.stepsPerUnit) — against the QUANTISED curvature
     * |da|/dist, not against κ. The two agree only up to rounding, and on a
     * subdivided ramp |da| is small enough that half a step is tens of percent.
     *
     * Where they disagree the floor wins silently: the packet runs slower than
     * planned, its neighbour does not, and the delivered speed steps by more
     * than dvMax with nothing in sample space aware of it. Measured on a
     * 10mm/8mm knife snake: 138 dvMax violations, 69 of them between two
     * packets that were BOTH at the A cap — the delivered speed was tracking
     * dist/|da| noise, not the plan.
     *
     * Feeding the measurement back closes the loop. Once v ≤ dist/tRate the
     * floor no longer binds, so the delivered speed IS the planned speed and
     * plan's own accel-continuity carries the smoothness. The pass only ever
     * lowers ceilings, so the iteration descends and terminates.
     */
    readonly measuredCeilings?: readonly number[];
}

// ── internal helpers ──────────────────────────────────────────────────────────

/**
 * GRBL junction-deviation cornering speed for a tangent turn of turn_deg across
 * a near-zero-length boundary. Models the corner as a circular arc deviating
 * from the exact vertex by at most `deviation` mm; returns the speed holding
 * centripetal accel at a_lat on that arc. Straight -> feedMax; reversal -> 0.
 *
 * Exported for direct testing (monotonicity, straight=feedMax) — the `_` prefix
 * in the Python original signaled "internal" but it's testable in isolation.
 */
export function junctionCap(
    turnDeg: number,
    aLat: number,
    deviation: number,
    feedMax: number,
): number {
    const halfCos = Math.cos((Math.abs(turnDeg) * Math.PI / 180) / 2);
    if (halfCos >= 1 - 1e-9) return feedMax;
    if (halfCos <= 1e-9) return 0;
    const radius = (deviation * halfCos) / (1 - halfCos);
    return Math.min(feedMax, Math.sqrt(aLat * radius));
}

/**
 * |dκ/ds| at sample i by central difference, 0 where it would straddle a
 * κ-discontinuity (curve/subpath boundary) or a ~zero-length span. Units: κ is
 * rad/mm, s is mm, so κ' is rad/mm².
 */
function kappaPrime(samples: readonly Sample[], i: number): number {
    const n = samples.length;
    if (i === 0 || i === n - 1) return 0;
    const s = samples[i]!;
    if (s.flags & KAPPA_BREAK || samples[i + 1]!.flags & KAPPA_BREAK) return 0;
    const span = samples[i - 1]!.ds + samples[i]!.ds; // arc length i-1 -> i+1
    if (span < 1e-6) return 0;
    return Math.abs(samples[i + 1]!.kappa - samples[i - 1]!.kappa) / span;
}

// ── main stage ────────────────────────────────────────────────────────────────

/**
 * Assign each sample a local velocity ceiling. Pure — no mutation; returns a
 * fresh ConstrainedSample[] (each entry is {...s, vCeiling: cap}).
 */
export function constrain(
    samples: readonly Sample[],
    options: ConstrainOptions,
): ConstrainedSample[] {
    const {
        feedMax,
        aMax,
        junctionDeviation,
        aRateDegS = 0,
        aAccelDegS2 = 0,
        cornerStopAngleDeg,
        forcedStops,
        vMin = 0,
        measuredCeilings,
    } = options;

    const aRateRad = aRateDegS > 0 ? (aRateDegS * Math.PI) / 180 : 0;
    const aAccRad = aAccelDegS2 > 0 ? (aAccelDegS2 * Math.PI) / 180 : 0;

    return samples.map((s, i) => {
        // Checked first and returned immediately: a forced stop is an OVERRIDE,
        // not another candidate ceiling to min() against. Nothing below can
        // raise a zero, but going through the motions would invite a later edit
        // to reorder the min() chain and quietly resurrect the sample.
        if (forcedStops?.has(i)) return { ...s, vCeiling: 0 };

        let cap = feedMax;
        if (s.kappa > 1e-9) {
            cap = Math.min(cap, Math.sqrt(aMax / s.kappa)); // centripetal (XY)
            if (aRateRad > 0) {
                cap = Math.min(cap, aRateRad / s.kappa); // A slew (velocity)
            }
        }

        // A angular-accel, curvature-gradient term: v <= sqrt(α_max / |κ'|)
        if (aAccRad > 0) {
            const kp = kappaPrime(samples, i);
            if (kp > 1e-9) {
                cap = Math.min(cap, Math.sqrt(aAccRad / kp));
            }
        }

        // Tangent jump — the corner signal.
        //
        // The STOP test is ungated (audit F2/D4): `discretize` computes dtheta
        // between consecutive samples with no flag test at all, so an
        // intra-curve cusp is a corner there. Gating this half on
        // CURVE_BOUNDARY meant discretize inserted a lift-pivot-lower at a
        // sample plan had never decelerated into — the two stages disagreed
        // about what a corner is, and discretize was the one that was right.
        //
        // The junction-deviation cap stays gated. It models a VERTEX between
        // two curves across a near-zero-length span; applying it to ordinary
        // in-curve samples would double-count the centripetal cap, which
        // already owns continuous turning.
        if (i > 0) {
            const turn = angleDelta(samples[i - 1]!.theta, s.theta);
            if (cornerStopAngleDeg !== undefined && Math.abs(turn) >= cornerStopAngleDeg) {
                cap = 0;
            } else if ((s.flags & CURVE_BOUNDARY) && Math.abs(turn) > 1e-6) {
                cap = Math.min(cap, junctionCap(turn, aMax, junctionDeviation, feedMax));
            }
        }

        // The packet-space measurement, if a previous pass made one. Applied
        // last of the ceilings but before the vMin test, so a measured ceiling
        // below the execution floor becomes an honest stop like any other.
        const measured = measuredCeilings?.[i];
        if (measured !== undefined && measured >= 0 && measured < cap) cap = measured;

        // A ceiling under the floor the machine will actually execute is a stop
        // (audit C1). Forcing it to 0 makes plan decelerate into it and
        // accelerate out, so the executed profile is the planned one.
        if (vMin > 0 && cap < vMin) cap = 0;

        return { ...s, vCeiling: cap };
    });
}
