/**
 * slots.test.ts — the four wire slots.
 *
 * The interesting cases are all dual-head, because that is where slots 2 and 3
 * are contended and where the demos' assumptions were wrong.
 */

import { describe, it, expect } from "vitest";
import {
    SLOT,
    axisSlots,
    slotMapFor,
    headForSlotMap,
} from "../../src/machine/slots.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    ToolType,
} from "../../src/machine/schema.js";
import { KNIFE, PEN } from "../../src/machine/tools.js";
import { defaultMachine, twoHeadMachine } from "../machines.js";
import { headsAccepting } from "../../src/machine/resolve.js";

/** Two heads on distinct Z/A nodes — knife on head 0, pen on head 1. */
function dualHead() {
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        [
            toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667, { rotary: true }), {
                accepts: [ToolType.KNIFE],
                xOffset: -50,
            }),
            toolHead(axisConfig(busNode(5), 1200), axisConfig(busNode(6), 51.667, { rotary: true }), {
                accepts: [ToolType.PEN],
                xOffset: 50,
            }),
        ],
    );
}

describe("axisSlots", () => {
    it("gives both heads' Z/A the same slots — the contention that forces engagement", () => {
        const rows = axisSlots(dualHead());
        const z = rows.filter((r) => r.letter === "z");
        expect(z).toHaveLength(2);
        expect(z.every((r) => r.slot === SLOT.Z)).toBe(true);
        expect(z.map((r) => r.head)).toEqual([0, 1]);
        // ...and distinct nodes behind them, which is why it matters.
        expect(z.map((r) => r.axis.node.id)).toEqual([3, 5]);
    });

    it("labels heads apart but leaves the gantry bare", () => {
        expect(axisSlots(dualHead()).map((r) => r.label)).toEqual([
            "X",
            "Y",
            "Z0",
            "A0",
            "Z1",
            "A1",
        ]);
    });

    it("keys are stable and unique, so a UI can diff rebuilds", () => {
        const keys = axisSlots(dualHead()).map((r) => r.key);
        expect(new Set(keys).size).toBe(keys.length);
        expect(keys).toEqual(["x", "y", "h0z", "h0a", "h1z", "h1a"]);
    });

    it("keeps absent nodes as rows flagged not-present, rather than dropping them", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160),
            [toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4, { present: false }), 51.667))],
        );
        const a = axisSlots(m).find((r) => r.key === "h0a")!;
        expect(a).toBeDefined();
        expect(a.present).toBe(false);
    });

    it("carries the calibration the caller would otherwise dig out of the axis", () => {
        const z = axisSlots(defaultMachine()).find((r) => r.key === "h0z")!;
        expect(z.stepsPerUnit).toBe(1200);
        expect(z.invert).toBe(true);
        expect(z.unit).toBe("mm");
        expect(axisSlots(defaultMachine()).find((r) => r.key === "h0a")!.unit).toBe("deg");
    });
});

describe("slotMapFor", () => {
    it("binds the chosen head's Z/A, leaving the gantry alone", () => {
        const m = dualHead();
        expect(slotMapFor(m, 0)).toEqual([1, 2, 3, 4]);
        expect(slotMapFor(m, 1)).toEqual([1, 2, 5, 6]);
    });

    it("binds an absent node as null, not as its id", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160),
            [toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4, { present: false }), 51.667))],
        );
        expect(slotMapFor(m, 0)).toEqual([1, 2, 3, null]);
    });

    it("yields nulls for a head that does not exist rather than throwing", () => {
        // Callers reach here from a stale UI index; a map of nulls is a safe
        // "engage nothing", where a throw would take down a render.
        expect(slotMapFor(dualHead(), 7)).toEqual([1, 2, null, null]);
    });
});

describe("headForSlotMap", () => {
    it("recovers the head from a committed map", () => {
        const m = dualHead();
        expect(headForSlotMap(m, [1, 2, 5, 6])).toBe(1);
        expect(headForSlotMap(m, [1, 2, 3, 4])).toBe(0);
    });

    it("returns null for an unbound or unrecognisable map", () => {
        const m = dualHead();
        expect(headForSlotMap(m, null)).toBeNull();
        expect(headForSlotMap(m, [null, null, null, null])).toBeNull();
        expect(headForSlotMap(m, [1, 2, 3, 6])).toBeNull(); // half of each head
    });
});

// headAssignment() is gone. It answered "which head seeds this tool" from
// config, which the seed field could only guess at; where a tool COULD go is
// now headsAccepting() (resolve.ts) and where it IS is headWithTool() (setup.ts).
describe("headsAccepting", () => {
    it("lists every head whose fixture takes the tool, in machine order", () => {
        const m = twoHeadMachine();
        expect(headsAccepting(m, ToolType.KNIFE)).toEqual([0]);
        expect(headsAccepting(m, ToolType.PEN)).toEqual([1]);
        expect(headsAccepting(m, ToolType.CREASE)).toEqual([0, 1]);
    });

    it("returns empty for a tool no head accepts", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160),
            [toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667))],
        );
        expect(headsAccepting(m, ToolType.KNIFE)).toEqual([]);
    });
});
