/**
 * Plan stage (redesign stage 6): the look-ahead feedrate planner.
 * Ported from pipeline/stages/plan_lookahead.py.
 *
 * Turns each sample's LOCAL vCeiling (from Constrain) into a globally
 * reachable speed v[i] via two sweeps over the sample stream of a subpath:
 *
 *   backward (decel feasibility), last -> first:
 *       v[i] = min(vCeiling[i], sqrt(v[i+1]^2 + 2*a*ds[i]))
 *   forward (accel feasibility), first -> last:
 *       v[i] = min(v[i],         sqrt(v[i-1]^2 + 2*a*ds[i-1]))
 *
 * After both, every sample's speed is simultaneously reachable-from-behind
 * and stoppable-ahead, so the profile is acceleration-continuous by
 * construction — the tile-era accel-continuity residual at curve junctions
 * cannot occur, because junctions are no longer planning boundaries (a
 * sample mid-stream is identical to one at a curve seam).
 *
 * Boundary conditions: PATH_START and PATH_END force v = 0 (the tool
 * starts/stops at rest around the pen-up jog between subpaths). Corner-stop
 * samples already carry vCeiling = 0 from Constrain; the zero-length gap on
 * either side propagates the stop to both neighbours — the lift-pivot
 * precondition.
 *
 * PER-AXIS acceleration: the accel over a segment is the tool-path
 * acceleration that keeps every axis within its own limit. For unit travel
 * direction (ux, uy), a_seg = min(x.accel/|ux|, y.accel/|uy|) — on a
 * diagonal each axis stays capped while the tool accelerates faster than a
 * single scalar would allow. Axes with accel = 0 are treated as unlimited;
 * if none constrain, the scalar aMax is used (square-machine parity). A-axis
 * angular accel: the tool speeding up while curved drives A angular accel
 * α = κ·a_tan, so a_tan <= rad(a.accel)/κ — the κ·a_tangential term that
 * pairs with Constrain's curvature-gradient velocity ceiling. Z accel is
 * not handled here (the sample stream is 2D in-cut; Z motion is
 * choreography in stage 7).
 *
 * Pure stage: takes PlanOptions (a focused subset of MachineConfig), never
 * imports config. The caller bridges config to stage at the call site.
 */

import { PATH_START, PATH_END, type Sample } from "./sample.js";
import type { ConstrainedSample } from "./constrain.js";

// ── types ─────────────────────────────────────────────────────────────────────

/**
 * Plan stage output: a ConstrainedSample with resolved speed v filled in.
 *
 * Completes the type progression:
 *   Sample → constrain → ConstrainedSample → plan → PlannedSample
 * The compiler enforces that constrain runs before plan (plan takes
 * ConstrainedSample[], not bare Sample[]), so a skipped constrain can't
 * silently produce infinite-speed planning.
 */
export interface PlannedSample extends ConstrainedSample {
    readonly v: number;
}

/**
 * Plan stage options.
 *
 * Four parameters from one config tier — useful to know when constructing
 * overrides at the call site:
 *
 *   MachineConfig  -> xAccel (X), yAccel (Y), aAccelDegS2 (A), aMax (scalar fallback)
 *
 * xAccel/yAccel/aAccelDegS2 = 0 means "unlimited" (that axis's accel term
 * is skipped). aMax is the scalar fallback used when no per-axis term
 * constrains (typically = x.accel for square machines).
 */
export interface PlanOptions {
    /** X axis accel ceiling (mm/s²); 0 = unlimited. Source: AxisConfig.maxAccel (X). */
    readonly xAccel: number;
    /** Y axis accel ceiling (mm/s²); 0 = unlimited. Source: AxisConfig.maxAccel (Y). */
    readonly yAccel: number;
    /** A axis angular accel ceiling (deg/s²); 0 = skip A term. Source: AxisConfig.maxAccel (A). */
    readonly aAccelDegS2: number;
    /** Scalar fallback accel (mm/s²). Source: min(X,Y maxAccel). */
    readonly aMax: number;
    /**
     * Commanded path-accel target (mm/s²). 0/undefined = no cap (use the
     * per-axis-derived limit). When set, caps the segment accel below what the
     * per-axis ceilings alone would allow. Source: tool.path.accel ?? machine.path.accel.
     */
    readonly pathAccel?: number;
}

// ── internal helpers ──────────────────────────────────────────────────────────

/**
 * Yield [start, end] inclusive index ranges, one per PATH_START..PATH_END.
 * Exported for testing (Python's _subpath_ranges).
 */
export function* subpathRanges(
    samples: readonly Sample[],
): Generator<[number, number]> {
    let start: number | null = null;
    for (let i = 0; i < samples.length; i++) {
        const s = samples[i]!;
        if (s.flags & PATH_START) start = i;
        if (s.flags & PATH_END) {
            if (start === null) start = i;
            yield [start, i];
            start = null;
        }
    }
}

/**
 * Tool-path accel over the segment s0->s1 honouring per-axis accel limits.
 * Exported for testing (Python's _seg_accel).
 *
 * X and Y: the tool accel projects onto each axis as a*|u_axis|, so to keep
 * each within its own limit, a <= min(x.accel/|ux|, y.accel/|uy|).
 *
 * A (tangential tracking): the tool speeding up while curved drives A
 * angular accel α = κ·a_tan, so a_tan <= rad(a.accel)/κ. This is the
 * κ·a_tangential term that pairs with Constrain's curvature-gradient
 * velocity ceiling — together they bound A's total angular acceleration.
 * aAccelDegS2 == 0 (unset) skips it.
 */
export function segAccel(
    s0: Sample,
    s1: Sample,
    options: PlanOptions,
): number {
    const { xAccel, yAccel, aAccelDegS2, aMax, pathAccel } = options;
    const dx = s1.x - s0.x;
    const dy = s1.y - s0.y;
    const d = Math.hypot(dx, dy);
    if (d < 1e-12) return aMax;
    const ux = Math.abs(dx) / d;
    const uy = Math.abs(dy) / d;
    const cands: number[] = [];
    if (xAccel > 0 && ux > 1e-9) cands.push(xAccel / ux);
    if (yAccel > 0 && uy > 1e-9) cands.push(yAccel / uy);
    if (aAccelDegS2 > 0) {
        const kap = Math.max(s0.kappa, s1.kappa);
        if (kap > 1e-9) cands.push((aAccelDegS2 * Math.PI) / 180 / kap);
    }
    // A tool that commands a lower path-accel caps the per-axis-derived limit.
    // Unset (0/undefined) leaves the derivation untouched (byte-neutral).
    if (pathAccel !== undefined && pathAccel > 0) cands.push(pathAccel);
    return cands.length > 0 ? Math.min(...cands) : aMax;
}

// ── main stage ────────────────────────────────────────────────────────────────

/**
 * Resolve sample.v via the backward+forward look-ahead. Pure — no mutation;
 * returns a fresh PlannedSample[] (each entry is {...s, v: computed}).
 */
export function plan(
    samples: readonly ConstrainedSample[],
    options: PlanOptions,
): PlannedSample[] {
    // Every sample must belong to a bracketed subpath, or the sweeps below
    // silently skip it and its vCeiling is returned verbatim — full feed from
    // a standing start, no error (audit P2). flatten always brackets, but plan
    // is an exported pure stage and the port gives it callers that are not
    // flatten (jog, streamed tiles). Cheap to check here, expensive to retrofit
    // after a caller has been built on the silence.
    let next = 0;
    for (const [lo, hi] of subpathRanges(samples)) {
        if (lo !== next) {
            throw new Error(
                `plan: samples [${next}, ${lo - 1}] are outside any PATH_START/PATH_END ` +
                `bracket and would be left unplanned`,
            );
        }
        next = hi + 1;
    }
    if (next !== samples.length) {
        throw new Error(
            `plan: samples [${next}, ${samples.length - 1}] are outside any ` +
            `PATH_START/PATH_END bracket and would be left unplanned` +
            (samples.length > 0 ? " (unterminated subpath — missing PATH_END?)" : ""),
        );
    }

    // Work on a mutable v array indexed by sample position; fold into
    // PlannedSample at the end. This mirrors the Python in-place mutation
    // pattern but on a local array — the input ConstrainedSample[] is never
    // touched.
    const v = samples.map((s) => s.vCeiling);

    for (const [lo, hi] of subpathRanges(samples)) {
        // init from ceilings; pin the endpoints to rest
        for (let i = lo; i <= hi; i++) v[i] = samples[i]!.vCeiling;
        v[lo] = 0;
        v[hi] = 0;

        // precompute per-segment accel (segment i links sample i and i+1)
        const aSeg = new Array<number>(hi).fill(0);
        for (let i = lo; i < hi; i++) {
            aSeg[i] = segAccel(samples[i]!, samples[i + 1]!, options);
        }

        // backward: ensure we can brake to each downstream speed
        for (let i = hi - 1; i >= lo; i--) {
            const ds = samples[i]!.ds;
            const reachable = Math.sqrt(v[i + 1]! * v[i + 1]! + 2 * aSeg[i]! * ds);
            if (reachable < v[i]!) v[i] = reachable;
        }

        // forward: ensure we can accelerate up to each speed
        for (let i = lo + 1; i <= hi; i++) {
            const ds = samples[i - 1]!.ds;
            const reachable = Math.sqrt(v[i - 1]! * v[i - 1]! + 2 * aSeg[i - 1]! * ds);
            if (reachable < v[i]!) v[i] = reachable;
        }

        // endpoints stay pinned (forward pass may have lifted hi off 0)
        v[lo] = 0;
        v[hi] = 0;
    }

    return samples.map((s, i) => ({ ...s, v: v[i]! }));
}
