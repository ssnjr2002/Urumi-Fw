/**
 * Mock curve fixtures for the pipeline tests (flatten / constrain / plan).
 * Ported from pipeline/data/mock_curves.py.
 *
 * Each case is a list of CubicBezier curves in mm coordinates, as produced by
 * stage 3, paired with analytically-known arc length / curvature where
 * possible so tests can assert real numbers.
 */

import { cubic, KAPPA, type CubicBezier } from "../../src/toolpath/geometry.js";

function pt(x: number, y: number): { x: number; y: number } {
    return { x, y };
}

// ── case 1: straight line ─────────────────────────────────────────────────────
// Degenerate cubic, control points collinear.
// Expected arc length: 100.0 mm exactly.
// Expected curvature:  0.0 everywhere.

export const STRAIGHT_LINE: CubicBezier[] = [
    cubic(pt(0, 0), pt(33.333, 0), pt(66.667, 0), pt(100, 0)),
];

// ── case 2: quarter circle radius 50mm ───────────────────────────────────────
// Standard cubic Bezier approximation of a quarter circle.
// Handle length = r * KAPPA
// Expected arc length: π * 50 / 2 ≈ 78.540 mm  (error <0.1%)
// Expected curvature:  ≈ 1/50 = 0.020 mm⁻¹ throughout

const R50 = 50.0;
const H50 = R50 * KAPPA;

export const QUARTER_CIRCLE_R50: CubicBezier[] = [
    cubic(pt(R50, 0), pt(R50, H50), pt(H50, R50), pt(0, R50)),
];

// ── case 3: tight quarter circle radius 5mm ──────────────────────────────────
// Same shape, 10× smaller. High curvature case.
// Expected arc length: π * 5 / 2 ≈ 7.854 mm
// Expected curvature:  ≈ 1/5 = 0.200 mm⁻¹

const R5 = 5.0;
const H5 = R5 * KAPPA;

export const QUARTER_CIRCLE_R5: CubicBezier[] = [
    cubic(pt(R5, 0), pt(R5, H5), pt(H5, R5), pt(0, R5)),
];

// ── case 4: S-curve ───────────────────────────────────────────────────────────
// Two opposing arcs. Curvature changes sign at the inflection point.
// kappa_max should be similar on both halves; overall path length ~arc of each.
// No clean analytical value — used to verify kappa sign change is detected.

export const S_CURVE: CubicBezier[] = [
    cubic(pt(0, 0),    pt(20, 40),   pt(40, 40),    pt(60, 0)),
    cubic(pt(60, 0),   pt(80, -40),  pt(100, -40),  pt(120, 0)),
];

// ── case 5: very short curve (1mm) ────────────────────────────────────────────
// Tests numerical stability at near-degenerate scale.
// Expected arc length: ≈ 1.0 mm

export const SHORT_CURVE: CubicBezier[] = [
    cubic(pt(0, 0), pt(0.333, 0), pt(0.667, 0), pt(1.0, 0)),
];

// ── case 6: long gentle arc ───────────────────────────────────────────────────
// Low curvature throughout, long path. Tests that Gauss-Legendre handles
// a slow-bending 500mm curve without error accumulation.
// Rough expected arc length: slightly > 500mm (curve bows out by 50mm at peak).

export const LONG_GENTLE_ARC: CubicBezier[] = [
    cubic(pt(0, 0), pt(166.667, 50), pt(333.333, 50), pt(500, 0)),
];

// ── case 7: near-cusp (very high curvature) ───────────────────────────────────
// Control points arranged so the curve nearly doubles back on itself.
// kappa_max should be very large (small radius of curvature).

export const NEAR_CUSP: CubicBezier[] = [
    cubic(pt(0, 0), pt(40, 0), pt(41, 1), pt(1, 1)),
];

// ── case 8: full circle (four quarter arcs) ───────────────────────────────────
// Four quarter-circle cubics joined into a full circle of radius 30mm.
// Expected total arc length: 2 * π * 30 ≈ 188.496 mm
// Expected curvature: ≈ 1/30 ≈ 0.0333 mm⁻¹ throughout

const R30 = 30.0;
const H30 = R30 * KAPPA;

export const FULL_CIRCLE_R30: CubicBezier[] = [
    cubic(pt(R30, 0),     pt(R30, H30),     pt(H30, R30),      pt(0, R30)),
    cubic(pt(0, R30),     pt(-H30, R30),    pt(-R30, H30),     pt(-R30, 0)),
    cubic(pt(-R30, 0),    pt(-R30, -H30),   pt(-H30, -R30),    pt(0, -R30)),
    cubic(pt(0, -R30),    pt(H30, -R30),    pt(R30, -H30),     pt(R30, 0)),
];

// ── case 9: exact cusp ────────────────────────────────────────────────────────
// B'(0.5) = 0 exactly: the tangent REVERSES through a point of zero speed.
// Derived from the cubic derivative at t=0.5,
//   0.25(p1-p0) + 0.5(p2-p1) + 0.25(p3-p2) = 0,
// which for p0=(0,0) reduces to p1x = p2x + p3x and p3y = -p2y.
//
// Deliberately NOT in the CASES registry. Four test files iterate CASES, and
// adding a cusp there would change constrain / plan / discretize expectations
// before those stages have been audited. Stages opt in by importing this
// directly, so a cusp regression is always attributable to one stage.
//
// This is the geometry that breaks flatten's tangent cap: at t=0.5 both
// `speed > 1e-12` and `kappa > 1e-9` are false, so the step-size caps that
// depend on them are skipped entirely (docs/planner_audit.md F1).

export const CUSP: CubicBezier[] = [
    cubic(pt(0, 0), pt(10, 0), pt(0, 5), pt(10, -5)),
];

// ── registry ──────────────────────────────────────────────────────────────────

export interface CurveCase {
    readonly curves: CubicBezier[];
    readonly expected: {
        readonly arcLength: number | null;
        readonly kappaMax: number | null;
    };
}

export const CASES: Readonly<Record<string, CurveCase>> = {
    straight_line:       { curves: STRAIGHT_LINE,       expected: { arcLength: 100.0,           kappaMax: 0.0 } },
    quarter_circle_r50:  { curves: QUARTER_CIRCLE_R50,  expected: { arcLength: (Math.PI * 50) / 2, kappaMax: 1 / 50 } },
    quarter_circle_r5:   { curves: QUARTER_CIRCLE_R5,   expected: { arcLength: (Math.PI * 5) / 2,  kappaMax: 1 / 5 } },
    s_curve:             { curves: S_CURVE,             expected: { arcLength: null,             kappaMax: null } },
    short_curve:         { curves: SHORT_CURVE,         expected: { arcLength: 1.0,              kappaMax: 0.0 } },
    long_gentle_arc:     { curves: LONG_GENTLE_ARC,     expected: { arcLength: null,             kappaMax: null } },
    near_cusp:           { curves: NEAR_CUSP,           expected: { arcLength: null,             kappaMax: null } },
    full_circle_r30:     { curves: FULL_CIRCLE_R30,     expected: { arcLength: 2 * Math.PI * 30, kappaMax: 1 / 30 } },
};
