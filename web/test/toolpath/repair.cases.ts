/**
 * Mock input data for stage 3 (C1 continuity enforcement).
 * Ported from pipeline/data/mock_stage3.py.
 *
 * Each case is a list of CubicBezier curves in mm coordinates,
 * as produced by stage 2.
 */

import { cubic, type CubicBezier } from "../../src/toolpath/geometry.js";

function pt(x: number, y: number): { x: number; y: number } {
    return { x, y };
}

// ── case 1: perfect C1 join ───────────────────────────────────────────────────
// p2 of [0], p3 of [0] / p0 of [1], p1 of [1] are collinear AND equidistant.
// Stage 3 should pass this through untouched.
export const C1_PERFECT: CubicBezier[] = [
    cubic(pt(0, 0),   pt(10, 20), pt(20, 30), pt(30, 30)),
    cubic(pt(30, 30), pt(40, 30), pt(50, 20), pt(60, 0)),
];

// ── case 2: C0 only, tangent direction break ──────────────────────────────────
// Endpoints match but the tangent turns sharply at the join (~90°).
export const C0_ONLY_SHARP_CORNER: CubicBezier[] = [
    cubic(pt(0, 0),  pt(10, 0), pt(20, 0), pt(30, 0)),
    cubic(pt(30, 0), pt(30, 10), pt(30, 20), pt(30, 30)),
];

// ── case 3: G1 but not C1 (collinear, unequal speed) ─────────────────────────
// Direction is continuous but the handle lengths differ.
export const G1_NOT_C1: CubicBezier[] = [
    cubic(pt(0, 0),  pt(10, 0), pt(20, 0), pt(30, 0)),
    cubic(pt(30, 0), pt(50, 0), pt(60, 0), pt(70, 0)),
];

// ── case 4: position gap (C0 broken) ─────────────────────────────────────────
// p3 of [0] != p0 of [1] — there is a spatial gap.
export const C0_BROKEN_GAP: CubicBezier[] = [
    cubic(pt(0, 0),   pt(10, 0), pt(20, 0), pt(30, 0)),
    cubic(pt(40, 10), pt(50, 10), pt(60, 10), pt(70, 10)),
];

// ── case 5: multiple consecutive bad joins ────────────────────────────────────
// Three curves, two joins both broken. Tests that repair iterates correctly.
export const MULTI_BAD_JOINS: CubicBezier[] = [
    cubic(pt(0, 0),   pt(10, 0),  pt(20, 0),  pt(30, 0)),
    cubic(pt(30, 0),  pt(30, 10), pt(30, 20), pt(30, 30)),
    cubic(pt(30, 30), pt(20, 30), pt(10, 30), pt(0, 30)),
];

// ── case 6: single curve (no join to check) ───────────────────────────────────
// Stage 3 must handle a length-1 list without crashing.
export const SINGLE_CURVE: CubicBezier[] = [
    cubic(pt(0, 0), pt(10, 20), pt(20, 20), pt(30, 0)),
];

// ── case 7: near-C1 within tolerance ─────────────────────────────────────────
// Tangent angle deviation is small (~4°) — below the 5° threshold.
export const NEAR_C1_WITHIN_TOLERANCE: CubicBezier[] = [
    cubic(pt(0, 0),  pt(10, 0),       pt(20, 0),       pt(30, 0)),
    cubic(pt(30, 0), pt(40, 0.698),   pt(50, 0.698),   pt(60, 0)),
];

// ── case 8: anti-parallel tangents (180° — a cusp) ───────────────────────────
// Exit tangent and entry tangent point in exactly opposite directions.
export const CUSP: CubicBezier[] = [
    cubic(pt(0, 0),  pt(10, 0), pt(20, 0), pt(30, 0)),
    cubic(pt(30, 0), pt(20, 0), pt(10, 0), pt(0, 0)),
];

// ── registry ──────────────────────────────────────────────────────────────────

export const CASES: Readonly<Record<string, CubicBezier[]>> = {
    c1_perfect: C1_PERFECT,
    c0_only_sharp_corner: C0_ONLY_SHARP_CORNER,
    g1_not_c1: G1_NOT_C1,
    c0_broken_gap: C0_BROKEN_GAP,
    multi_bad_joins: MULTI_BAD_JOINS,
    single_curve: SINGLE_CURVE,
    near_c1_within_tolerance: NEAR_C1_WITHIN_TOLERANCE,
    cusp: CUSP,
};
