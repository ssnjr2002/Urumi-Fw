/**
 * Tests for job.ts — an SVG layer to BEZIER packets.
 */

import { describe, it, expect } from "vitest";
import { prepareBezierJob, type BezierJob } from "../../src/toolpath/job.js";
import { BezierFlag } from "../../src/toolpath/annotate.js";
import { packBezier } from "../../src/wire/format/bezier.js";

const svg = (body: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" width="100mm" height="100mm" viewBox="0 0 100 100">${body}</svg>`;
const SQUARE = `<path d="M10 10 h20 v20 h-20 Z"/>`;
const TWO = SQUARE + `<path d="M50 50 C60 40 70 60 80 50"/>`;

const job = (body: string, layer = "", options = {}): BezierJob => {
    const j = prepareBezierJob(svg(body), layer, options);
    if ("error" in j) throw new Error(j.error);
    return j;
};

describe("prepareBezierJob", () => {
    it("moves the drawing by the offset", () => {
        expect(job(SQUARE, "", { offset: { x: 5, y: 7 } }).bbox).toEqual({ minX: 15, minY: 77, maxX: 35, maxY: 97 });
    });

    it.each([
        ["a missing layer", SQUARE, "knife", /no layer/],
        ["a layer with nothing to cut", `<path d="M1 1 L1 1"/>`, "", /nothing to cut/],
    ])("refuses %s", (_, body, layer, err) => {
        expect(prepareBezierJob(svg(body), layer)).toEqual({ error: expect.stringMatching(err) });
    });

    it("frames each contour with START and END", () => {
        for (const c of job(TWO).contours) {
            expect(c[0]!.flags & BezierFlag.START).toBeTruthy();
            expect(c.at(-1)!.flags & BezierFlag.END).toBeTruthy();
            expect(c.slice(1).every((b) => !(b.flags & BezierFlag.START))).toBe(true);
        }
    });

    it("passes the quality tolerances to clean", () => {
        const breaks = (angleTol: number) =>
            job(SQUARE, "", { quality: { angleTol, gapTol: 0.01 } }).contours[0]!.filter((b) => b.flags & BezierFlag.BREAK).length;
        expect(breaks(5)).toBe(3);
        expect(breaks(95)).toBe(0);
    });

    it("packs one packet per piece, in order", () => {
        const j = job(TWO);
        expect(j.packets).toEqual(j.contours.flat().map((b) => packBezier(b)));
    });
});
