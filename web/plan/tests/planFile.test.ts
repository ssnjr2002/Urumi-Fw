/**
 * Tests for plan/planFile — the slot-aware .plan codec.
 * Round-trip (save → load → identical), header/manifest bytes, slot encoding,
 * and the truncation / bad-magic / unknown-tool error paths.
 */

import { describe, it, expect } from "vitest";

import { microSegment } from "../../wire/src/microsegment.js";
import { PEN, KNIFE, REVOLVER_PEN } from "../../config/config.js";
import type { Plan } from "../src/plan.js";
import { planToolTypes } from "../src/plan.js";
import {
    savePlan,
    loadPlan,
    PLAN_MAGIC,
    PLAN_VERSION,
    SLOT_NONE,
} from "../src/planFile.js";

const seg = (dx: number, dy: number, flags = 0) => microSegment(dx, dy, 0, 0, 1000, flags);

const samplePlan = (): Plan => ({
    blocks: [
        { profile: KNIFE, segments: [seg(10, 0), seg(0, 10, 0x01)] },
        { profile: REVOLVER_PEN, slot: 0, segments: [seg(5, 5)] },
        { profile: REVOLVER_PEN, slot: 2, segments: [seg(-3, 4), seg(1, 1), seg(0, 0, 0x01)] },
    ],
});

describe("plan/planFile: round-trip", () => {
    it("save → load reproduces the plan exactly", () => {
        const plan = samplePlan();
        const loaded = loadPlan(savePlan(plan));
        expect(loaded.blocks.length).toBe(plan.blocks.length);
        loaded.blocks.forEach((b, i) => {
            const orig = plan.blocks[i]!;
            expect(b.profile.toolType).toBe(orig.profile.toolType);
            expect(b.slot).toBe(orig.slot);
            expect(b.segments).toEqual(orig.segments);
        });
    });

    it("preserves revolver slot indices (0 and 2)", () => {
        const loaded = loadPlan(savePlan(samplePlan()));
        expect(loaded.blocks[1]!.slot).toBe(0);
        expect(loaded.blocks[2]!.slot).toBe(2);
    });

    it("non-revolver blocks have no slot", () => {
        const loaded = loadPlan(savePlan(samplePlan()));
        expect(loaded.blocks[0]!.slot).toBeUndefined();
    });
});

describe("plan/planFile: header + manifest", () => {
    it("writes the slot-aware magic and version", () => {
        const bytes = savePlan(samplePlan());
        expect([...bytes.slice(0, 4)]).toEqual([...PLAN_MAGIC]);
        expect(bytes[4]).toBe(PLAN_VERSION);
    });

    it("manifest lists unique tool types in first-appearance order", () => {
        const bytes = savePlan(samplePlan());
        const nTools = bytes[5]!;
        const nOps = bytes[6]! | (bytes[7]! << 8);
        expect(nOps).toBe(3);
        expect(nTools).toBe(2); // KNIFE, REVOLVER_PEN
        const manifest = [...bytes.slice(8, 8 + nTools)];
        expect(manifest).toEqual(planToolTypes(samplePlan()) as number[]);
    });

    it("encodes SLOT_NONE (0xFF) for a non-slot block", () => {
        // first op header starts at 8 + nTools; slot byte is the 2nd byte
        const bytes = savePlan({ blocks: [{ profile: PEN, segments: [seg(1, 1)] }] });
        const nTools = bytes[5]!;
        const opStart = 8 + nTools;
        expect(bytes[opStart + 1]).toBe(SLOT_NONE);
    });
});

describe("plan/planFile: error paths", () => {
    it("rejects bad magic", () => {
        const bytes = savePlan(samplePlan());
        bytes[0] = 0x00;
        expect(() => loadPlan(bytes)).toThrow(/magic/);
    });

    it("rejects an unsupported version", () => {
        const bytes = savePlan(samplePlan());
        bytes[4] = 0x99;
        expect(() => loadPlan(bytes)).toThrow(/version/);
    });

    it("rejects a truncated file", () => {
        const bytes = savePlan(samplePlan());
        expect(() => loadPlan(bytes.slice(0, bytes.length - 5))).toThrow(/truncated/);
    });

    it("rejects a corrupted packet CRC", () => {
        const bytes = savePlan(samplePlan());
        const last = bytes.length - 1;
        bytes[last] = (bytes[last]! ^ 0xff) & 0xff; // flip the last packet's CRC byte
        expect(() => loadPlan(bytes)).toThrow(/CRC/);
    });

    it("handles an empty plan", () => {
        const loaded = loadPlan(savePlan({ blocks: [] }));
        expect(loaded.blocks).toEqual([]);
    });
});
