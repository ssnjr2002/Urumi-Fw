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
 * The three "always needed" values are required. The three "disable switches"
 * are optional — their absence naturally means "disabled" (no A cap, no
 * forced corner stops). No config import, no hidden defaults; the caller
 * bridges all three tiers.
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
    } = options;

    const aRateRad = aRateDegS > 0 ? (aRateDegS * Math.PI) / 180 : 0;
    const aAccRad = aAccelDegS2 > 0 ? (aAccelDegS2 * Math.PI) / 180 : 0;

    return samples.map((s, i) => {
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

        // Tangent jump across a curve boundary (the corner signal)
        if ((s.flags & CURVE_BOUNDARY) && i > 0) {
            const turn = angleDelta(samples[i - 1]!.theta, s.theta);
            if (cornerStopAngleDeg !== undefined && Math.abs(turn) >= cornerStopAngleDeg) {
                cap = 0;
            } else if (Math.abs(turn) > 1e-6) {
                cap = Math.min(cap, junctionCap(turn, aMax, junctionDeviation, feedMax));
            }
        }

        return { ...s, vCeiling: cap };
    });
}
