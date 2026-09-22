/**
 * Sample — the spine of the redesigned pipeline (stages 4-8).
 *
 * A Sample is one point along a flattened toolpath: position, tangent, local
 * curvature, and the arc-length step to the NEXT sample. After the Flatten
 * stage the whole job is a single flat Sample[]; every downstream stage
 * operates on that list instead of on Bezier tiles.
 *
 * Sample carries only GEOMETRY — no velocity fields. Downstream stages
 * produce richer types: ConstrainedSample (adds vCeiling) and PlannedSample
 * (adds v). Each stage's output type documents its guarantee; the compiler
 * catches skipped stages.
 *
 * Angle convention: theta is in DEGREES (matches steps_per_deg and the
 * tangent helpers in geometry.ts); curvature kappa is in 1/mm (a geometric
 * quantity, angle-unit independent).
 */

export interface Sample {
    readonly x: number;       // mm, machine frame
    readonly y: number;       // mm, machine frame
    readonly theta: number;   // tangent angle, degrees
    readonly kappa: number;   // local curvature, 1/mm (>= 0)
    readonly ds: number;      // arc length to next sample, mm (0 at subpath end)
    readonly flags: number;   // PATH_START | PATH_END | CURVE_BOUNDARY
}

// ── provenance flags ──────────────────────────────────────────────────────────
// These mark WHERE a sample came from; the tool-dependent CORNER decision (is a
// curve-boundary tangent jump sharp enough to lift-pivot?) is made downstream
// where the ToolProfile is known, not here.

export const PATH_START = 0x01;   // first sample of a subpath (planner forces v_entry = 0)
export const PATH_END = 0x02;     // last sample of a subpath  (planner forces v_exit  = 0)
export const CURVE_BOUNDARY = 0x04; // first sample of a curve following another in the
                                    // same subpath. The previous sample is the prior
                                    // curve's t=1; the two share a position but may
                                    // differ in tangent — that difference IS the
                                    // corner signal.

export function sample(
    x: number,
    y: number,
    theta: number,
    kappa: number,
    ds: number,
    flags = 0,
): Sample {
    return { x, y, theta, kappa, ds, flags };
}
