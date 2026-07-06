/**
 * Tests for the Constrain stage (redesign stage 5): per-sample velocity ceiling.
 * Ported from pipeline/stages/test_constrain.py.
 *
 * The test is the caller — it sources config values inline (FEED, A_MAX, etc.,
 * matching the Python test constants) and passes them via ConstrainOptions.
 * constrain() never imports config.
 */

import { describe, it, expect } from "vitest";
import { lineToCubic } from "../src/geometry.js";
import { flatten } from "../src/flatten.js";
import { constrain, junctionCap } from "../src/constrain.js";
import { CURVE_BOUNDARY } from "../src/sample.js";
import { CASES } from "./data/curves.cases.js";
import { qualityConfig } from "../../config/config.js";

const FEED = 80.0;
const A_MAX = 1000.0;
const q = qualityConfig();

// ── straight -> feed_max everywhere ───────────────────────────────────────────

describe("stage 5: straight line", () => {
    it("ceiling == feed_max everywhere", () => {
        const s = flatten([CASES.straight_line!.curves], q);
        const c = constrain(s, {
            feedMax: FEED,
            aMax: A_MAX,
            junctionDeviation: q.junctionDeviation,
        });
        for (const x of c) {
            expect(Math.abs(x.vCeiling - FEED)).toBeLessThan(1e-6);
        }
    });
});

// ── circle -> constant centripetal cap ────────────────────────────────────────

describe("stage 5: centripetal cap", () => {
    it("r5 circle: kappa=0.2 -> v ≈ sqrt(1000/0.2) ≈ 70.7 < feed", () => {
        const s = flatten([CASES.quarter_circle_r5!.curves], q);
        const c = constrain(s, {
            feedMax: FEED,
            aMax: A_MAX,
            junctionDeviation: q.junctionDeviation,
        });
        const expected = Math.sqrt(A_MAX / 0.2);
        for (const x of c) {
            expect(x.vCeiling).toBeLessThanOrEqual(FEED + 1e-6);
        }
        const mid = c[Math.floor(c.length / 2)]!.vCeiling;
        expect(Math.abs(mid - expected) / expected).toBeLessThan(0.05);
    });

    it("tighter circle -> lower cap", () => {
        const s50 = flatten([CASES.quarter_circle_r50!.curves], q);
        const s5 = flatten([CASES.quarter_circle_r5!.curves], q);
        const c50 = constrain(s50, { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation });
        const c5 = constrain(s5, { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation });
        const mid50 = c50[Math.floor(c50.length / 2)]!.vCeiling;
        const mid5 = c5[Math.floor(c5.length / 2)]!.vCeiling;
        expect(mid5).toBeLessThan(mid50);
    });
});

// ── A-slew cap ────────────────────────────────────────────────────────────────

describe("stage 5: A-slew cap", () => {
    it("slow A axis lowers ceiling on tight curve (rad(100)/0.2)", () => {
        const baseOpts = { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation };
        const sNo = flatten([CASES.quarter_circle_r5!.curves], q);
        const sA = flatten([CASES.quarter_circle_r5!.curves], q);
        const cNo = constrain(sNo, baseOpts);
        const cA = constrain(sA, { ...baseOpts, aRateDegS: 100.0 });
        const midNo = cNo[Math.floor(cNo.length / 2)]!.vCeiling;
        const midA = cA[Math.floor(cA.length / 2)]!.vCeiling;
        expect(midA).toBeLessThan(midNo);
        expect(Math.abs(midA - (Math.PI * 100 / 180) / 0.2) / midA).toBeLessThan(0.05);
    });
});

// ── A-accel curvature-gradient cap ────────────────────────────────────────────

describe("stage 5: A-accel gradient cap", () => {
    it("changing curvature (s_curve) — tight a_accel lowers min ceiling", () => {
        const baseOpts = { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation };
        const s0 = flatten([CASES.s_curve!.curves], q);
        const s1 = flatten([CASES.s_curve!.curves], q);
        const c0 = constrain(s0, baseOpts);
        const c1 = constrain(s1, { ...baseOpts, aAccelDegS2: 50.0 });
        const min0 = c0.reduce((m, x) => Math.min(m, x.vCeiling), Infinity);
        const min1 = c1.reduce((m, x) => Math.min(m, x.vCeiling), Infinity);
        expect(min1).toBeLessThan(min0);
    });

    it("constant curvature — A-accel cap inactive (dk/ds = 0)", () => {
        const baseOpts = { feedMax: FEED, aMax: A_MAX, junctionDeviation: q.junctionDeviation };
        const s0 = flatten([CASES.quarter_circle_r50!.curves], q);
        const s1 = flatten([CASES.quarter_circle_r50!.curves], q);
        const c0 = constrain(s0, baseOpts);
        const c1 = constrain(s1, { ...baseOpts, aAccelDegS2: 50.0 });
        const mid = Math.floor(c0.length / 2);
        expect(Math.abs(c0[mid]!.vCeiling - c1[mid]!.vCeiling)).toBeLessThan(1e-9);
    });
});

// ── corner stop ───────────────────────────────────────────────────────────────

describe("stage 5: corner stop", () => {
    it("sharp corner forces vCeiling = 0", () => {
        const horiz = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        const vert = lineToCubic({ x: 10, y: 0 }, { x: 10, y: 10 });
        const s = flatten([[horiz, vert]], q);
        const c = constrain(s, {
            feedMax: FEED,
            aMax: A_MAX,
            junctionDeviation: q.junctionDeviation,
            cornerStopAngleDeg: 20.0,
        });
        const bi = c.findIndex((x) => x.flags & CURVE_BOUNDARY);
        expect(c[bi]!.vCeiling).toBe(0);
    });

    it("no corner stop when disabled — junction cap still applies", () => {
        const horiz = lineToCubic({ x: 0, y: 0 }, { x: 10, y: 0 });
        const vert = lineToCubic({ x: 10, y: 0 }, { x: 10, y: 10 });
        const s = flatten([[horiz, vert]], q);
        const c = constrain(s, {
            feedMax: FEED,
            aMax: A_MAX,
            junctionDeviation: q.junctionDeviation,
            // cornerStopAngleDeg omitted — no forced stops
        });
        const bi = c.findIndex((x) => x.flags & CURVE_BOUNDARY);
        expect(c[bi]!.vCeiling).toBeGreaterThan(0);
        expect(c[bi]!.vCeiling).toBeLessThan(FEED);
    });
});

// ── junction cap helper ───────────────────────────────────────────────────────

describe("stage 5: junctionCap helper", () => {
    it("monotone — sharper turn -> lower cap, straight = feedMax", () => {
        const straight = junctionCap(1.0, A_MAX, 0.05, FEED);
        const gentle = junctionCap(30.0, A_MAX, 0.05, FEED);
        const sharp = junctionCap(120.0, A_MAX, 0.05, FEED);
        expect(straight).toBeGreaterThanOrEqual(gentle);
        expect(gentle).toBeGreaterThanOrEqual(sharp);
        expect(junctionCap(0.0, A_MAX, 0.05, FEED)).toBe(FEED);
    });
});

// ── ceiling never exceeds feed ────────────────────────────────────────────────

describe("stage 5: ceiling bounded by feed", () => {
    it("all cases — vCeiling <= feedMax", () => {
        for (const [name, { curves }] of Object.entries(CASES)) {
            const s = flatten([curves], q);
            const c = constrain(s, {
                feedMax: FEED,
                aMax: A_MAX,
                junctionDeviation: q.junctionDeviation,
                aRateDegS: 100.0,
                cornerStopAngleDeg: 20.0,
            });
            for (const x of c) {
                if (x.vCeiling > FEED + 1e-6) {
                    throw new Error(`${name}: vCeiling ${x.vCeiling} > feed ${FEED}`);
                }
            }
        }
    });
});
