/**
 * Tests for config/frames — the home↔tool coordinate transforms and the
 * anchor/offset accessors they read from config.
 */

import { describe, it, expect } from "vitest";
import {
    machineAnchor,
    headOffset,
    toolFrameOffset,
    headSeparation,
    homeToTool,
    toolToHome,
    stepsToUnits,
    unitsToSteps,
    homePosition,
    type XY,
} from "../../src/config/frames.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    toolProfile,
    type MachineConfig,
    type ToolProfile,
} from "../../src/config/config.js";

/** Dual-head machine: head 0 at the origin, head 1 offset by (dx, dy). */
function machine(opts?: {
    offsets?: readonly XY[];
    laser?: { xOffset: number; yOffset: number } | undefined;
}): MachineConfig {
    const offsets = opts?.offsets ?? [{ x: 0, y: 0 }, { x: 60, y: 0 }];
    const heads = offsets.map((o, i) =>
        toolHead(
            axisConfig(busNode(10 + i * 2), 600),
            axisConfig(busNode(11 + i * 2), 45.46, { rotary: true }),
            { xOffset: o.x, yOffset: o.y },
        ),
    );
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        heads,
        opts?.laser !== undefined ? { laser: opts.laser } : undefined,
    );
}

/** A tool whose tip sits off the head centre. */
function tool(xOffset: number, yOffset: number): ToolProfile {
    return toolProfile("probe", { toolOffset: { xOffset, yOffset } });
}

describe("config/frames: the anchor", () => {
    it("is the head mounted at (0,0) when no laser is fitted", () => {
        expect(machineAnchor(machine())).toEqual({ kind: "head", index: 0 });
    });

    it("is the laser whenever one is fitted, even if a head is also at (0,0)", () => {
        const m = machine({ laser: { xOffset: 0, yOffset: 0 } });
        expect(machineAnchor(m)).toEqual({ kind: "laser" });
    });

    it("finds the origin head wherever it sits in the array", () => {
        const m = machine({ offsets: [{ x: -60, y: 0 }, { x: 0, y: 0 }] });
        expect(machineAnchor(m)).toEqual({ kind: "head", index: 1 });
    });

    it("reports `none` when every head is offset away from the origin", () => {
        // Legal, but it means no hardware sits at (0,0) — validation's business,
        // not a reason to guess a head here.
        const m = machine({ offsets: [{ x: -30, y: 0 }, { x: 30, y: 0 }] });
        expect(machineAnchor(m)).toEqual({ kind: "none" });
    });
});

describe("config/frames: offsets", () => {
    it("headOffset reads the head's mounting position", () => {
        const m = machine();
        expect(headOffset(m, 0)).toEqual({ x: 0, y: 0 });
        expect(headOffset(m, 1)).toEqual({ x: 60, y: 0 });
    });

    it("headOffset throws on an out-of-range index rather than yielding NaN", () => {
        // headIndex comes from runtime selection, not from config, so a bad one
        // is a caller bug that should surface here and not as a silent offset.
        expect(() => headOffset(machine(), 2)).toThrow(RangeError);
        expect(() => headOffset(machine(), -1)).toThrow(/out of range/);
    });

    it("toolFrameOffset adds the tool's tip offset to the head's", () => {
        const m = machine();
        expect(toolFrameOffset(m, 1, tool(2, -3))).toEqual({ x: 62, y: -3 });
    });

    it("toolFrameOffset with a centred tool equals headOffset", () => {
        const m = machine();
        expect(toolFrameOffset(m, 1, tool(0, 0))).toEqual(headOffset(m, 1));
    });

    it("headSeparation is the absolute distance, order-independent", () => {
        const m = machine({ offsets: [{ x: -20, y: 5 }, { x: 40, y: -5 }] });
        expect(headSeparation(m, 0, 1)).toEqual({ x: 60, y: 10 });
        expect(headSeparation(m, 1, 0)).toEqual({ x: 60, y: 10 });
    });
});

describe("config/frames: wire ↔ home", () => {
    const plain = { stepsPerUnit: 160 };
    const flipped = { stepsPerUnit: 160, invert: true };

    it("stepsToUnits converts by stepsPerUnit", () => {
        expect(stepsToUnits(16000, plain)).toBe(100);
        expect(stepsToUnits(-800, plain)).toBe(-5);
    });

    it("stepsToUnits does not round — a readout shows sub-unit drift", () => {
        expect(stepsToUnits(1, plain)).toBeCloseTo(0.00625, 10);
    });

    it("invert flips the sign in both directions", () => {
        expect(stepsToUnits(16000, flipped)).toBe(-100);
        expect(unitsToSteps(100, flipped)).toBe(-16000);
    });

    it("unitsToSteps rounds to a whole step", () => {
        expect(unitsToSteps(0.001, plain)).toBe(0); // 0.16 steps
        expect(unitsToSteps(0.01, plain)).toBe(2); // 1.6 steps
    });

    it("unitsToSteps matches what jogToPoint computes for a target", () => {
        // Same expression as jogTo.ts:65 — if these diverge, a go-to lands on a
        // different step than the readout claims.
        const target = 37.5;
        const inv = flipped.invert ? -1 : 1;
        expect(unitsToSteps(target, flipped)).toBe(
            Math.round(target * flipped.stepsPerUnit * inv),
        );
    });

    it("homePosition reads X/Y out of a STATUS_RSP pos array", () => {
        const m = machine(); // x plain, y plain — both 160 steps/mm
        expect(homePosition(m, [16000, 8000, 999, 999])).toEqual({ x: 100, y: 50 });
    });

    it("homePosition applies each axis's own inversion", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160, { invert: true }),
            [toolHead(axisConfig(busNode(3), 600), axisConfig(busNode(4), 45.46))],
        );
        expect(homePosition(m, [16000, 8000, 0, 0])).toEqual({ x: 100, y: -50 });
    });

    it("homePosition tolerates a short or empty pos array", () => {
        // STATUS_RSP always carries four, but `pos` is optional on the parsed
        // status and a caller may poll before the first reply lands.
        expect(homePosition(machine(), [])).toEqual({ x: 0, y: 0 });
    });

    it("round-trips a position through wire and back", () => {
        const m = machine();
        const steps = [16000, 8000];
        const home = homePosition(m, steps);
        expect(unitsToSteps(home.x, m.x)).toBe(steps[0]);
        expect(unitsToSteps(home.y, m.y)).toBe(steps[1]);
    });
});

describe("config/frames: home ↔ tool", () => {
    it("homeToTool shifts the machine position out to the tip", () => {
        expect(homeToTool({ x: 100, y: 50 }, { x: 60, y: 0 })).toEqual({ x: 160, y: 50 });
    });

    it("toolToHome answers where to command so the tip lands on target", () => {
        expect(toolToHome({ x: 160, y: 50 }, { x: 60, y: 0 })).toEqual({ x: 100, y: 50 });
    });

    it("the two are inverses", () => {
        const offset = { x: 60, y: -12.5 };
        const pos = { x: 33.3, y: 7.25 };
        expect(toolToHome(homeToTool(pos, offset), offset)).toEqual(pos);
    });

    it("the anchor's tip reads the machine position unchanged", () => {
        const m = machine();
        const pos = { x: 100, y: 50 };
        expect(homeToTool(pos, headOffset(m, 0))).toEqual(pos);
    });

    it("a tip coordinate may be negative while the machine position is not", () => {
        // Head at -60 with the machine at +10: the tip is behind the origin.
        // Signs are a property of the frame, not an error.
        const m = machine({ offsets: [{ x: 0, y: 0 }, { x: -60, y: 0 }] });
        expect(homeToTool({ x: 10, y: 0 }, headOffset(m, 1))).toEqual({ x: -50, y: 0 });
    });

    it("switching the viewed head changes the reading, not the machine", () => {
        // The whole point of the frame model: one physical position, two numbers.
        const m = machine();
        const pos = { x: 100, y: 50 };
        expect(homeToTool(pos, headOffset(m, 0))).toEqual({ x: 100, y: 50 });
        expect(homeToTool(pos, headOffset(m, 1))).toEqual({ x: 160, y: 50 });
    });

    it("go-to on head 1 commands the machine short by the offset", () => {
        // Operator asks for tip X=0 on the offset head; the machine must sit at
        // -60 to put it there.
        const m = machine();
        expect(toolToHome({ x: 0, y: 0 }, headOffset(m, 1))).toEqual({ x: -60, y: 0 });
    });
});
