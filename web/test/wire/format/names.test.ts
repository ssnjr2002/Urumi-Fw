/**
 * names.test.ts — enum names, and the reason they are derived not literal.
 */

import { describe, it, expect } from "vitest";
import {
    stateName,
    alarmName,
    runningName,
    maskStr,
    STATE_NAMES,
} from "../../../src/wire/format/names.js";
import {
    MachineState,
    AlarmReason,
    RunningReason,
    axisMask,
} from "../../../src/wire/format/status.js";

describe("enum names", () => {
    it("names every state the enum declares, by its own key", () => {
        for (const [key, value] of Object.entries(MachineState)) {
            expect(stateName(value)).toBe(key);
        }
    });

    it("covers alarm and running reasons the same way", () => {
        for (const [key, value] of Object.entries(AlarmReason)) {
            expect(alarmName(value)).toBe(key);
        }
        for (const [key, value] of Object.entries(RunningReason)) {
            expect(runningName(value)).toBe(key);
        }
    });

    it("is keyed by value, so it survives a reorder of the enum declaration", () => {
        // The positional-array version the demos used got this wrong: it maps
        // by *position*, so ALARM=3 only reads "ALARM" while ALARM happens to
        // be the fourth key written down.
        expect(STATE_NAMES[MachineState.ALARM]).toBe("ALARM");
        expect(STATE_NAMES[MachineState.PAUSED]).toBe("PAUSED");
    });

    it("keeps an unknown value visible instead of rendering undefined", () => {
        expect(stateName(99)).toBe("STATE(99)");
        expect(alarmName(99)).toBe("ALARM(99)");
        expect(runningName(99)).toBe("RUNNING(99)");
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
