/**
 * Tests for stage 3: C1 continuity enforcement.
 * Ported from host/production/test_repair.py.
 *
 * The test is the caller — it bridges config to the stage by sourcing
 * default tolerances from qualityConfig() and passing them in via the
 * RepairOptions object. The stage itself never imports config.
 */

import { describe, it, expect } from "vitest";
import { readFixture } from "../helpers.js";
import {
    angleBetweenDeg,
    exitTangent,
    entryTangent,
    length,
    sub,
    type Pt,
} from "../../src/toolpath/geometry.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { CASES } from "./repair.cases.js";
import { qualityConfig } from "../../src/machine/index.js";
import { loadSvgMm } from "../../src/svg/ingest.js";


function svg(name: string): string {
    return readFixture(name);
}

function approxPt(a: Pt, b: Pt, tol = 1e-6): boolean {
    return Math.abs(a.x - b.x) < tol && Math.abs(a.y - b.y) < tol;
}

// Default tolerances sourced at the test boundary (the caller side).
const q = qualityConfig();
const defaultOpts = { angleTolDeg: q.angleTol, gapTolMm: q.gapTol };

// ── passthrough cases ─────────────────────────────────────────────────────────

describe("stage 3: passthrough", () => {
    it("perfect C1 untouched", () => {
        const { repaired, logs } = enforceC1(CASES.c1_perfect!, defaultOpts);
        expect(repaired).toHaveLength(2);
        expect(logs).toHaveLength(0);
    });

    it("G1 not C1 untouched (direction continuous)", () => {
        const { repaired, logs } = enforceC1(CASES.g1_not_c1!, defaultOpts);
        expect(repaired).toHaveLength(2);
        expect(logs).toHaveLength(0);
    });

    it("near-C1 within tolerance untouched", () => {
        const { repaired, logs } = enforceC1(CASES.near_c1_within_tolerance!, defaultOpts);
        expect(repaired).toHaveLength(2);
        expect(logs).toHaveLength(0);
    });

    it("single curve untouched", () => {
        const { repaired, logs } = enforceC1(CASES.single_curve!, defaultOpts);
        expect(repaired).toHaveLength(1);
        expect(logs).toHaveLength(0);
    });

    it("empty list", () => {
        const { repaired, logs } = enforceC1([], defaultOpts);
        expect(repaired).toEqual([]);
        expect(logs).toEqual([]);
    });
});

// ── repair cases ──────────────────────────────────────────────────────────────

describe("stage 3: repairs", () => {
    it("sharp corner logged as cusp, no curve inserted", () => {
        const { repaired, logs } = enforceC1(CASES.c0_only_sharp_corner!, defaultOpts);
        expect(repaired).toHaveLength(2);
        expect(logs).toHaveLength(1);
        expect(logs[0]!.kind).toBe("cusp");
        expect(Math.abs(logs[0]!.angleDeg - 90)).toBeLessThan(0.1);
    });

    it("gap inserts bridge", () => {
        const { repaired, logs } = enforceC1(CASES.c0_broken_gap!, defaultOpts);
        expect(repaired).toHaveLength(3);
        expect(logs).toHaveLength(1);
        expect(logs[0]!.kind).toBe("bridge");
        expect(logs[0]!.gapMm).toBeGreaterThan(0.01);
    });

    it("multi bad joins — both logged as cusp", () => {
        const { repaired, logs } = enforceC1(CASES.multi_bad_joins!, defaultOpts);
        expect(repaired).toHaveLength(3);
        expect(logs).toHaveLength(2);
        expect(logs.every((l) => l.kind === "cusp")).toBe(true);
    });

    it("cusp logged not modified", () => {
        const { repaired, logs } = enforceC1(CASES.cusp!, defaultOpts);
        expect(repaired).toHaveLength(2);
        expect(logs).toHaveLength(1);
        expect(logs[0]!.kind).toBe("cusp");
        expect(Math.abs(logs[0]!.angleDeg - 180)).toBeLessThan(0.1);
    });
});

// ── blend cubic geometry ──────────────────────────────────────────────────────

describe("stage 3: blend cubic geometry", () => {
    it("endpoints preserved after bridge insertion", () => {
        const { repaired } = enforceC1(CASES.c0_broken_gap!, defaultOpts);
        const original = CASES.c0_broken_gap!;
        expect(approxPt(repaired[0]!.p0, original[0]!.p0)).toBe(true);
        expect(approxPt(repaired[0]!.p3, original[0]!.p3)).toBe(true);
        expect(approxPt(repaired[2]!.p0, original[1]!.p0)).toBe(true);
        expect(approxPt(repaired[2]!.p3, original[1]!.p3)).toBe(true);
    });

    it("bridge connects at join (p0 == prev.p3, p3 == next.p0)", () => {
        const { repaired } = enforceC1(CASES.c0_broken_gap!, defaultOpts);
        const bridge = repaired[1]!;
        expect(approxPt(bridge.p0, repaired[0]!.p3)).toBe(true);
        expect(approxPt(bridge.p3, repaired[2]!.p0)).toBe(true);
    });

    it("bridge p1 lies along exit tangent of preceding curve", () => {
        const { repaired } = enforceC1(CASES.c0_broken_gap!, defaultOpts);
        const exitT = exitTangent(repaired[0]!);
        const bridge = repaired[1]!;
        const handle = sub(bridge.p1, bridge.p0);
        const handleDir: Pt = { x: handle.x / length(handle), y: handle.y / length(handle) };
        expect(angleBetweenDeg(exitT, handleDir)).toBeLessThan(1.0);
    });

    it("bridge p2 lies along entry tangent of following curve", () => {
        const { repaired } = enforceC1(CASES.c0_broken_gap!, defaultOpts);
        const entryT = entryTangent(repaired[2]!);
        const bridge = repaired[1]!;
        const handle = sub(bridge.p3, bridge.p2);
        const handleDir: Pt = { x: handle.x / length(handle), y: handle.y / length(handle) };
        expect(angleBetweenDeg(entryT, handleDir)).toBeLessThan(1.0);
    });
});

// ── angle tolerance boundary ──────────────────────────────────────────────────

describe("stage 3: tolerance boundary", () => {
    it("exactly at tolerance — not repaired", () => {
        const { logs } = enforceC1(CASES.near_c1_within_tolerance!, {
            angleTolDeg: 5.0,
            gapTolMm: q.gapTol,
        });
        expect(logs).toHaveLength(0);
    });

    it("tighter tolerance triggers repair", () => {
        const { logs } = enforceC1(CASES.near_c1_within_tolerance!, {
            angleTolDeg: 2.0,
            gapTolMm: q.gapTol,
        });
        expect(logs).toHaveLength(1);
    });
});

// ── real SVG passthrough ──────────────────────────────────────────────────────

describe("stage 3: real SVG", () => {
    it("snake.svg — no repairs", () => {
        const { curves } = loadSvgMm(svg("test_snake.svg"));
        const { repaired, logs } = enforceC1(curves, defaultOpts);
        expect(logs).toHaveLength(0);
        expect(repaired).toHaveLength(curves.length);
    });
});
