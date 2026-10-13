/**
 * names.test.ts — enum names, and the reason they are derived not literal.
 */

import { describe, it, expect } from "vitest";
import {
    stateName,
    faultNames,
    jobName,
    maskStr,
    STATE_NAMES,
} from "../../../src/wire/format/names.js";
import {
    MachineState,
    Fault,
    JobReason,
    axisMask,
} from "../../../src/wire/format/status.js";

describe("enum names", () => {
    it("names every state the enum declares, by its own key", () => {
        for (const [key, value] of Object.entries(MachineState)) {
            expect(stateName(value)).toBe(key);
        }
    });

    it("names each fault bit, and a mask by its set bits", () => {
        for (const [key, value] of Object.entries(Fault)) {
            expect(faultNames(value)).toBe(key);
        }
        expect(faultNames(0)).toBe("NONE");
        expect(faultNames(Fault.UNMAPPED | Fault.LATCHED)).toBe("UNMAPPED|LATCHED");
        for (const [key, value] of Object.entries(JobReason)) {
            expect(jobName(value)).toBe(key);
        }
    });

    it("is keyed by value, so it survives a reorder of the enum declaration", () => {
        // The positional-array version the demos used got this wrong: it maps
        // by *position*, so ALARM=3 only reads "ALARM" while ALARM happens to
        // be the fourth key written down.
        expect(STATE_NAMES[MachineState.ALARM]).toBe("ALARM");
        expect(STATE_NAMES[MachineState.JOB]).toBe("JOB");
    });

    it("keeps an unknown value visible instead of rendering undefined", () => {
        expect(stateName(99)).toBe("STATE(99)");
        expect(faultNames(0x0100)).toBe("0x100");
        expect(jobName(99)).toBe("JOB(99)");
    });
});

describe("maskStr", () => {
    it("renders set bits as letters in axis order", () => {
        expect(maskStr(axisMask("xy"))).toBe("xy");
        expect(maskStr(axisMask("az"))).toBe("za"); // canonical order, not input order
        expect(maskStr(axisMask("xyza"))).toBe("xyza");
    });

    it("renders an empty mask as a dash, so it does not look like a render bug", () => {
        expect(maskStr(0)).toBe("—");
    });

    it("ignores bits above the four axes", () => {
        expect(maskStr(0xf0 | axisMask("x"))).toBe("x");
    });
});
