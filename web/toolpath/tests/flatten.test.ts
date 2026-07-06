/**
 * Tests for the Flatten stage (redesign stage 4): Bezier subpaths -> Sample stream.
 * Ported from pipeline/stages/test_flatten.py.
 *
 * Validates the sample stream reproduces stage4's geometry (arc length,
 * endpoints, curvature) — flatten is a re-representation, not a
 * re-computation, so it must agree with the trusted stage4 metrics.
 */

import { describe, it, expect } from "vitest";
import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { arcLength, lineToCubic, type CubicBezier } from "../src/geometry.js";
import { flatten } from "../src/flatten.js";
import { PATH_START, PATH_END, CURVE_BOUNDARY } from "../src/sample.js";
import { enforceC1 } from "../src/repair.js";
import { CASES } from "./data/curves.cases.js";
import { qualityConfig } from "../../config/config.js";
import { loadSvgMmSubpaths } from "../../svg/ingest.js";

const __dirname = dirname(fileURLToPath(import.meta.url));
const DATA = join(__dirname, "..", "..", "..", "pipeline", "data");

function svg(name: string): string {
    return readFileSync(join(DATA, name), "utf-8");
}

// Default quality sourced at the test boundary (the caller side).
const q = qualityConfig();

function analyticLen(curves: readonly CubicBezier[]): number {
    return curves.reduce((sum, c) => sum + arcLength(c), 0);
}

// ── arc length: sum of ds matches stage4 ──────────────────────────────────────

describe("stage 4: arc length", () => {
    it("total length matches stage4 analytic (all cases except near_cusp)", () => {
        for (const [name, { curves }] of Object.entries(CASES)) {
            if (name === "near_cusp") continue;
            const samples = flatten([curves], q);
            const chordTotal = samples.reduce((sum, s) => sum + s.ds, 0);
            const analytic = analyticLen(curves);
            const err = Math.abs(chordTotal - analytic) / analytic;
            if (err >= 0.005) {
                throw new Error(`${name}: chord len ${chordTotal} vs analytic ${analytic} (err ${err})`);
            }
        }
    });

    it("near_cusp length reasonable (chord sum >= GL5)", () => {
        const samples = flatten([CASES.near_cusp!.curves], q);
        const chordTotal = samples.reduce((sum, s) => sum + s.ds, 0);
        const gl5 = analyticLen(CASES.near_cusp!.curves);
        expect(chordTotal).toBeGreaterThanOrEqual(gl5 - 1e-6);
    });

    it("straight line length ≈ 100mm", () => {
        const samples = flatten([CASES.straight_line!.curves], q);
        const total = samples.reduce((sum, s) => sum + s.ds, 0);
        expect(Math.abs(total - 100.0)).toBeLessThan(0.01);
    });
});

// ── endpoints preserved ───────────────────────────────────────────────────────

describe("stage 4: endpoints", () => {
    it("first sample == first curve p0, last sample == last curve p3", () => {
        for (const [name, { curves }] of Object.entries(CASES)) {
            const samples = flatten([curves], q);
            const first = samples[0]!;
            const last = samples[samples.length - 1]!;
            if (Math.abs(first.x - curves[0]!.p0.x) >= 1e-9) {
                throw new Error(`${name}: first sample x ${first.x} vs ${curves[0]!.p0.x}`);
            }
            if (Math.abs(first.y - curves[0]!.p0.y) >= 1e-9) {
                throw new Error(`${name}: first sample y ${first.y} vs ${curves[0]!.p0.y}`);
            }
            if (Math.abs(last.x - curves[curves.length - 1]!.p3.x) >= 1e-6) {
                throw new Error(`${name}: last sample x ${last.x} vs ${curves[curves.length - 1]!.p3.x}`);
            }
            if (Math.abs(last.y - curves[curves.length - 1]!.p3.y) >= 1e-6) {
                throw new Error(`${name}: last sample y ${last.y} vs ${curves[curves.length - 1]!.p3.y}`);
            }
        }
    });
});

// ── curvature parity with stage4 ──────────────────────────────────────────────

describe("stage 4: curvature", () => {
    it("quarter circle r50 — kappa ≈ 0.02 at every sample (within 5%)", () => {
        const samples = flatten([CASES.quarter_circle_r50!.curves], q);
        for (const s of samples) {
            expect(Math.abs(s.kappa - 0.02)).toBeLessThan(0.02 * 0.05);
        }
    });

    it("straight line — max kappa < 1e-6", () => {
        const samples = flatten([CASES.straight_line!.curves], q);
        const maxKappa = samples.reduce((mx, s) => Math.max(mx, s.kappa), 0);
        expect(maxKappa).toBeLessThan(1e-6);
    });
});

// ── flags ─────────────────────────────────────────────────────────────────────

describe("stage 4: flags", () => {
    it("s_curve — exactly one PATH_START and one PATH_END", () => {
        const samples = flatten([CASES.s_curve!.curves], q);
        expect(samples[0]!.flags & PATH_START).toBeTruthy();
        expect(samples[samples.length - 1]!.flags & PATH_END).toBeTruthy();
        expect(samples.filter((s) => s.flags & PATH_START)).toHaveLength(1);
        expect(samples.filter((s) => s.flags & PATH_END)).toHaveLength(1);
    });

    it("s_curve — exactly one CURVE_BOUNDARY (start of 2nd curve)", () => {
        const samples = flatten([CASES.s_curve!.curves], q);
        expect(samples.filter((s) => s.flags & CURVE_BOUNDARY)).toHaveLength(1);
    });

    it("full circle — 3 internal CURVE_BOUNDARY (first curve has none)", () => {
        const samples = flatten([CASES.full_circle_r30!.curves], q);
        expect(samples.filter((s) => s.flags & CURVE_BOUNDARY)).toHaveLength(3);
    });
});

// ── multi-subpath ─────────────────────────────────────────────────────────────

describe("stage 4: multi-subpath", () => {
    it("two subpaths — 2 PATH_START, 2 PATH_END, ds=0 at boundary", () => {
        const a = CASES.straight_line!.curves;
        const b = CASES.quarter_circle_r50!.curves;
        const samples = flatten([a, b], q);
        expect(samples.filter((s) => s.flags & PATH_START)).toHaveLength(2);
        expect(samples.filter((s) => s.flags & PATH_END)).toHaveLength(2);
        // ds does not bridge subpaths: the last sample of subpath A has ds=0
        const starts = samples
            .map((s, i) => (s.flags & PATH_START ? i : -1))
            .filter((i) => i >= 0);
        expect(samples[starts[1]! - 1]!.ds).toBe(0);
    });
});

// ── spacing cap ───────────────────────────────────────────────────────────────

describe("stage 4: spacing", () => {
    it("ds_max respected — no sample step exceeds dsMax", () => {
        const samples = flatten([CASES.straight_line!.curves], q);
        for (let i = 0; i < samples.length - 1; i++) {
            expect(samples[i]!.ds).toBeLessThanOrEqual(q.dsMax + 1e-6);
        }
    });

    it("corner shows as tangent jump with ~zero ds", () => {
        const horiz = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        const vert = lineToCubic({ x: 10, y: 0 }, { x: 10, y: 10 });
        const samples = flatten([[horiz, vert]], q);
        const bi = samples.findIndex((s) => s.flags & CURVE_BOUNDARY);
        expect(bi).toBeGreaterThan(0);
        const jump = Math.abs(samples[bi]!.theta - samples[bi! - 1]!.theta);
        expect(Math.abs(jump - 90)).toBeLessThan(1.0);
        expect(samples[bi! - 1]!.ds).toBeLessThan(1e-6);
    });
});

// ── real SVG regression ───────────────────────────────────────────────────────

describe("stage 4: real SVG", () => {
    it("snake.svg — flatten after repair, total 150-200mm", () => {
        const { subpaths } = loadSvgMmSubpaths(svg("test_snake.svg"));
        const repairOpts = { angleTolDeg: q.angleTol, gapTolMm: q.gapTol };
        const repaired = subpaths.map((sp) => enforceC1(sp, repairOpts).repaired);
        const samples = flatten(repaired, q);
        const total = samples.reduce((sum, s) => sum + s.ds, 0);
        expect(total).toBeGreaterThan(150);
        expect(total).toBeLessThan(200);
        expect(samples.every((s) => typeof s.x === "number")).toBe(true);
    });
});
