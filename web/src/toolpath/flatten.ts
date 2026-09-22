/**
 * Flatten stage (redesign stage 4): repaired Bezier subpaths -> flat Sample[].
 * Ported from pipeline/stages/flatten.py.
 *
 * The representation drop the whole redesign turns on: after here the pipeline
 * no longer sees Bezier curves, only an arc-length sample stream carrying
 * PER-SAMPLE local curvature. The tile-era conservatism (one cruise speed per
 * curve, capped by the curve's MAX curvature) is structurally impossible once
 * the unit is the sample.
 *
 * Sampling is driven by three geometry-only limits (velocity is unknown here —
 * that is the point; planning happens downstream):
 *   1. chord deviation:  dt <= sqrt(8 * chordTol / |B''(t)|)   — finer where curved
 *   2. spacing cap:       dt <= dsMax / |B'(t)|                 — bounded everywhere
 *   3. tangent step:      dt <= dthetaMax / (kappa * |B'(t)|)   — bounded angular turn
 * The spacing cap guarantees enough samples on long straight runs for the
 * look-ahead's accel/decel ramps to be smooth, where chord deviation alone
 * would emit only a handful. The tangent cap keeps a tangential knife smooth
 * on curves: chord deviation alone allows a large tangent jog over a
 * low-deviation chord.
 *
 * Curve boundaries are kept as adjacent samples (prev curve t=1, next curve
 * t=0) with ds~0 between them: a smooth join is a harmless near-duplicate; a
 * sharp corner shows up as two samples with the SAME position but a large
 * theta jump — that jump is the corner signal the Constrain / Choreograph
 * stages read.
 *
 * Pure stage: takes FlattenOptions (a subset of QualityConfig), never imports
 * config. The caller sources values from qualityConfig() / overrides.
 */

import {
    bezierPoint,
    bezierDeriv1,
    bezierDeriv2,
    curvature,
    angleDelta,
    type CubicBezier,
} from "./geometry.js";
import { PATH_START, PATH_END, CURVE_BOUNDARY, type Sample } from "./sample.js";

export interface FlattenOptions {
    readonly chordTol: number;
    readonly dsMax: number;
    readonly dthetaMax: number;
    readonly dtMax: number;
    readonly dtMin: number;
    /**
     * How many times a step may be halved when the step it predicted turns out
     * to have overshot a cap (audit F1/F7). Each halving at most doubles the
     * samples in that neighbourhood, so this is the knob that bounds how many
     * samples a cusp can cost — the thing a sample-count-bounded window on the
     * Pico actually cares about.
     *
     * The firmware ships one fixed value; a host doing offline work can raise
     * it to resolve pathological geometry more finely. `dtMin` still applies
     * underneath as a hard floor.
     *
     * Absent = 0 = no enforcement, i.e. the pre-F7 behaviour where the three
     * caps were predictors rather than bounds.
     */
    readonly maxRefine?: number;
}

// ── internal helpers ──────────────────────────────────────────────────────────

function tangentDeg(c: CubicBezier, t: number, fallback = 0): number {
    const d1 = bezierDeriv1(c, t);
    if (d1.x * d1.x + d1.y * d1.y < 1e-20) return fallback;
    return (Math.atan2(d1.y, d1.x) * 180) / Math.PI;
}

/**
 * Geometry-only adaptive step: min of three caps (chord, spacing, tangent).
 *
 * PREDICTION ONLY. Every cap here is evaluated at the step's START and then
 * applied across the whole step, so where the curve speeds up or bends harder
 * over that interval the realised value overshoots — systematically, not as
 * float noise (`snake.svg`: 142 of 356 steps over `dsMax`). `tsForCurve` is
 * what turns these predictions into bounds.
 *
 * The epsilon guards are the other half of the problem (audit F1). Both are
 * gated on "is this quantity measurable", when at a cusp the correct
 * behaviour is the opposite: near-zero speed is exactly where the tangent is
 * least stable and the cap matters most. Left as they are here — deliberately,
 * because the fix belongs in the enforcement loop rather than in a second
 * epsilon — so a cusp yields dtMax from this function and gets cut down by
 * measurement instead of by prediction.
 */
function dtAt(
    c: CubicBezier,
    t: number,
    chordTol: number,
    dsMax: number,
    dthetaMax: number,
    dtMax: number,
): number {
    let dt = dtMax;
    const d1 = bezierDeriv1(c, t);
    const speed = Math.hypot(d1.x, d1.y);
    const d2 = bezierDeriv2(c, t);
    const mag2 = d2.x * d2.x + d2.y * d2.y;
    if (mag2 > 1e-20) {
        dt = Math.min(dt, Math.sqrt((8 * chordTol) / Math.sqrt(mag2)));
    }
    if (speed > 1e-12) {
        dt = Math.min(dt, dsMax / speed);
        const k = curvature(c, t);
        if (k > 1e-9) {
            dt = Math.min(dt, ((dthetaMax * Math.PI) / 180) / (k * speed));
        }
    }
    return dt;
}

/**
 * Parameter values [0..1] at which to sample one curve (both ends inclusive).
 *
 * Each candidate step is proposed by `dtAt` and then MEASURED: the realised
 * chord and the realised tangent turn are computed from the two endpoints, and
 * a step that overshot either cap is halved and re-measured (audit F7 option 1,
 * chosen deliberately over restating the caps as targets). Enforcement also
 * closes F1 without a second epsilon rule — a cusp, where `dtAt`'s guards skip
 * both caps and return `dtMax`, is now cut down by the turn it actually makes
 * instead of being stepped straight over.
 *
 * `maxRefine` bounds the halving, so a pathological curve costs bounded extra
 * samples rather than unbounded ones. On exhaustion the step is taken anyway:
 * the caps are enforced as far as the sample budget allows, and no further.
 */
function tsForCurve(
    c: CubicBezier,
    chordTol: number,
    dsMax: number,
    dthetaMax: number,
    dtMax: number,
    dtMin: number,
    maxRefine: number,
): number[] {
    const ts: number[] = [0];
    let t = 0;
    while (t < 1) {
        let dt = Math.max(dtMin, dtAt(c, t, chordTol, dsMax, dthetaMax, dtMax));
        if (maxRefine > 0) {
            const p0 = bezierPoint(c, t);
            const th0 = tangentDeg(c, t);
            let prevTurn = Infinity;
            for (let r = 0; r < maxRefine; r++) {
                const tEnd = Math.min(t + dt, 1);
                const p1 = bezierPoint(c, tEnd);
                const chord = Math.hypot(p1.x - p0.x, p1.y - p0.y);
                const turn = Math.abs(angleDelta(th0, tangentDeg(c, tEnd, th0)));
                if (chord <= dsMax + 1e-12 && turn <= dthetaMax + 1e-12) break;
                if (dt <= dtMin) break;
                // A TRUE cusp cannot be resolved by sampling harder: the tangent
                // reverses at a single parameter value, so the realised turn
                // tends to 180 deg no matter how small the step gets. Halving
                // further would buy samples and change nothing. Stop, and let
                // the 180 deg jump be read as the CORNER it is — constrain stops
                // there and discretize pivots (F2/C1). This is the correction to
                // F1's original premise that "a cusp must force fine sampling".
                if (chord <= dsMax + 1e-12 && turn > dthetaMax && turn >= prevTurn * 0.99) break;
                prevTurn = turn;
                dt = Math.max(dtMin, dt / 2);
            }
        }
        t = Math.min(t + dt, 1);
        ts.push(t);
    }
    return ts;
}

// ── main stage ────────────────────────────────────────────────────────────────

/**
 * Flatten repaired Bezier subpaths into one flat Sample[].
 *
 * Each inner list of subpaths is one continuous pen-down stroke.
 * PATH_START / PATH_END bracket each subpath; CURVE_BOUNDARY marks
 * intra-subpath curve joins. ds on each sample is the chord to the next
 * sample (0.0 on the final sample of each subpath).
 */
export function flatten(
    subpaths: readonly (readonly CubicBezier[])[],
    options: FlattenOptions,
): Sample[] {
    const { chordTol, dsMax, dthetaMax, dtMax, dtMin, maxRefine = 0 } = options;
    const out: Sample[] = [];

    for (const subpath of subpaths) {
        if (subpath.length === 0) continue;
        const subStart = out.length;
        let prevTheta = tangentDeg(subpath[0]!, 0);

        for (let ci = 0; ci < subpath.length; ci++) {
            const c = subpath[ci]!;
            const ts = tsForCurve(c, chordTol, dsMax, dthetaMax, dtMax, dtMin, maxRefine);
            for (let k = 0; k < ts.length; k++) {
                const t = ts[k]!;
                const p = bezierPoint(c, t);
                const theta = tangentDeg(c, t, prevTheta);
                const kappa = curvature(c, t);
                let flags = 0;
                if (ci > 0 && k === 0) flags |= CURVE_BOUNDARY;
                out.push({ x: p.x, y: p.y, theta, kappa, ds: 0, flags });
                prevTheta = theta;
            }
        }

        // mark subpath ends (replace-on-update — samples are readonly)
        out[subStart] = { ...out[subStart]!, flags: out[subStart]!.flags | PATH_START };
        out[out.length - 1] = { ...out[out.length - 1]!, flags: out[out.length - 1]!.flags | PATH_END };

        // fill ds = chord to next sample, within this subpath only
        for (let i = subStart; i < out.length - 1; i++) {
            const cur = out[i]!;
            const next = out[i + 1]!;
            out[i] = { ...cur, ds: Math.hypot(next.x - cur.x, next.y - cur.y) };
        }
        // last sample of subpath: ds stays 0 (set at creation)
    }

    return out;
}
