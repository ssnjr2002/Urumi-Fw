/**
 * names.test.ts — the wire codes in words, derived from the enums, not literal.
 */

import { describe, it, expect } from "vitest";
import * as names from "../../../src/packets/names.js";
import {
    MachineState,
    Fault,
    JobReason,
    JoggingReason,
    HomingReason,
    ProbingReason,
    MachineStatus,
    axisMask,
} from "../../../src/packets/status.js";

describe("names", () => {
    it("names every state the enum declares, by its own key", () => {
        // Keyed by value: the positional array the demos used read ALARM=3 as
        // "ALARM" only while ALARM was the fourth key written down.
        for (const [key, value] of Object.entries(MachineState)) {
            expect(names.state(value)).toBe(key);
        }
    });

    it("names each fault bit, and a mask by its set bits", () => {
        for (const [key, value] of Object.entries(Fault)) {
            expect(names.faults(value)).toBe(key);
        }
        expect(names.faults(0)).toBe("NONE");
        expect(names.faults(Fault.UNMAPPED | Fault.LATCHED)).toBe("UNMAPPED|LATCHED");
    });

    it("reads the reason against its state", () => {
        const byState = [
            [MachineState.JOB, JobReason],
            [MachineState.JOGGING, JoggingReason],
            [MachineState.HOMING, HomingReason],
            [MachineState.PROBING, ProbingReason],
        ] as const;
        for (const [st, e] of byState) {
            for (const [key, value] of Object.entries(e)) {
                expect(names.reason(st, value)).toBe(key);
            }
        }
        expect(names.reason(MachineState.JOB, 1)).toBe("RUNNING");
        expect(names.reason(MachineState.JOGGING, 1)).toBe("STEP");
        expect(names.reason(MachineState.IDLE, 0)).toBe("");
    });

    it("keeps an unknown value visible instead of rendering undefined", () => {
        expect(names.state(99)).toBe("STATE(99)");
        expect(names.faults(0x0200)).toBe("0x200");
        expect(names.reason(MachineState.JOB, 99)).toBe("REASON(99)");
        expect(names.nack(0x03)).toBe("NACK(3)");
    });
});

describe("names.selection", () => {
    it("names the anchor and none, and a head by its index", () => {
        expect([names.selection(0xfe), names.selection(0xff), names.selection(1)]).toEqual(["ANCHOR", "NONE", "1"]);
    });
});

describe("names.axes", () => {
    it("renders set bits as letters in axis order", () => {
        expect(names.axes(axisMask("xy"))).toBe("xy");
        expect(names.axes(axisMask("az"))).toBe("za"); // canonical order, not input order
        expect(names.axes(axisMask("xyza"))).toBe("xyza");
    });

    it("renders an empty mask as a dash, so it does not look like a render bug", () => {
        expect(names.axes(0)).toBe("—");
    });

    it("ignores bits above the four axes", () => {
        expect(names.axes(0xf0 | axisMask("x"))).toBe("x");
    });
});

describe("MachineStatus names", () => {
    it("names its state, reason and faults", () => {
        const s = new MachineStatus(MachineState.JOGGING, 0, 0, Fault.UNMAPPED, JoggingReason.GOTO);
        expect([s.stateName, s.reasonName, s.faultNames]).toEqual(["JOGGING", "GOTO", "UNMAPPED"]);
    });
});
