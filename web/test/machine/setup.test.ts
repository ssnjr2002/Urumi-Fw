/**
 * setup.test.ts — the live half of the machine model.
 *
 * The load-bearing case is setupAxes() on a dual-head machine after a switch:
 * that is the silent-wrong-calibration bug the Controller exists to prevent,
 * and it is the one place these functions differ from what the library did
 * before.
 */

import { describe, it, expect } from "vitest";
import {
    setupFor,
    engage,
    mount,
    engagedTool,
    headWithTool,
    isMounted,
    setupAxes,
    engagedAxis,
    setupSlotMap,
    isCommitted,
    adoptCommitted,
    sameSetup,
} from "../../src/machine/setup.js";
import { resolvedAxes } from "../../src/machine/resolve.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
} from "../../src/machine/schema.js";
import { KNIFE, PEN, CREASE } from "../../src/machine/tools.js";

/** Head 0: knife, Z at 1200 steps/mm. Head 1: pen, Z at 600 — deliberately 2x. */
function dualHead(defaultHead = 0) {
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        [
            toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667, { rotary: true }), {
                profile: KNIFE,
            }),
            toolHead(axisConfig(busNode(5), 600), axisConfig(busNode(6), 51.667, { rotary: true }), {
                profile: PEN,
            }),
        ],
        { defaultHead },
    );
}

describe("setupFor", () => {
    it("seeds from defaultHead and the sockets' seed profiles", () => {
        const s = setupFor(dualHead(1));
        expect(s.engaged).toBe(1);
        expect(s.mounted.map((p) => p?.toolType)).toEqual([KNIFE.toolType, PEN.toolType]);
    });

    it("represents an empty socket as null, not as a missing entry", () => {
        const m = machineConfig(axisConfig(busNode(1), 160), axisConfig(busNode(2), 160), [
            toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667)),
        ]);
        expect(setupFor(m).mounted).toEqual([null]);
    });

    it("agrees with the old static behaviour on a freshly seeded setup", () => {
        // Adopting Setup must be a no-op until someone actually switches heads,
        // or every existing caller changes meaning silently.
        const m = dualHead(0);
        expect(setupAxes(m, setupFor(m))).toEqual(resolvedAxes(m));
    });
});

describe("engage / mount", () => {
    it("returns a new value and leaves the original untouched", () => {
        const m = dualHead();
        const a = setupFor(m);
        const b = engage(m, a, 1);
        expect(a.engaged).toBe(0);
        expect(b.engaged).toBe(1);
    });

    it("returns the same object when engaging the already-engaged head", () => {
        const m = dualHead();
        const a = setupFor(m);
        expect(engage(m, a, 0)).toBe(a);
    });

    it("throws rather than engaging a head the machine does not have", () => {
        const m = dualHead();
        expect(() => engage(m, setupFor(m), 4)).toThrow(RangeError);
        expect(() => mount(m, setupFor(m), 4, PEN)).toThrow(RangeError);
    });

    it("mounts and empties a socket without touching the other", () => {
        const m = dualHead();
        const s = mount(m, setupFor(m), 0, CREASE);
        expect(s.mounted[0]).toBe(CREASE);
        expect(s.mounted[1]?.toolType).toBe(PEN.toolType);
        expect(mount(m, s, 1, null).mounted[1]).toBeNull();
    });
});

describe("tool queries reflect the setup, not the description", () => {
    it("engagedTool follows the engaged head", () => {
        const m = dualHead();
        const s = setupFor(m);
        expect(engagedTool(s)?.toolType).toBe(KNIFE.toolType);
        expect(engagedTool(engage(m, s, 1))?.toolType).toBe(PEN.toolType);
    });

    it("headWithTool finds what is fitted NOW, after a re-mount", () => {
        const m = dualHead();
        // The description says head 0 seeds a knife. Fit a crease instead.
        const s = mount(m, setupFor(m), 0, CREASE);
        expect(headWithTool(s, CREASE.toolType)).toBe(0);
        expect(headWithTool(s, KNIFE.toolType)).toBeNull();
        expect(isMounted(s, KNIFE.toolType)).toBe(false);
    });

    it("engagedTool is null for an empty socket", () => {
        const m = dualHead();
        expect(engagedTool(mount(m, setupFor(m), 0, null))).toBeNull();
    });
});

describe("setupAxes", () => {
    it("takes Z/A from the engaged head after a switch", () => {
        const m = dualHead(0);
        const switched = engage(m, setupFor(m), 1);
        expect(setupAxes(m, switched).z.stepsPerUnit).toBe(600);
        expect(setupAxes(m, switched).z.node.id).toBe(5);
    });

    it("differs from resolvedAxes() exactly when the engaged head is not the default", () => {
        // The bug, stated as a test: resolvedAxes keeps answering with the
        // default head's calibration, a clean 2x error on this machine.
        const m = dualHead(0);
        const switched = engage(m, setupFor(m), 1);
        expect(resolvedAxes(m).z.stepsPerUnit).toBe(1200);
        expect(setupAxes(m, switched).z.stepsPerUnit).toBe(600);
    });

    it("leaves the gantry and fCpu alone", () => {
        const m = dualHead();
        const s = setupAxes(m, engage(m, setupFor(m), 1));
        expect(s.x).toBe(m.x);
        expect(s.y).toBe(m.y);
        expect(s.fCpu).toBe(m.fCpu);
    });

    it("throws on a setup engaging a head that does not exist", () => {
        const m = dualHead();
        expect(() => setupAxes(m, { engaged: 9, mounted: [] })).toThrow(RangeError);
    });

    it("engagedAxis picks one letter out", () => {
        const m = dualHead();
        expect(engagedAxis(m, engage(m, setupFor(m), 1), "z").stepsPerUnit).toBe(600);
        expect(engagedAxis(m, setupFor(m), "x").stepsPerUnit).toBe(160);
    });
});

describe("reconciliation against the firmware's committed map", () => {
    it("setupSlotMap is what a commit would send", () => {
        const m = dualHead();
        expect(setupSlotMap(m, setupFor(m))).toEqual([1, 2, 3, 4]);
        expect(setupSlotMap(m, engage(m, setupFor(m), 1))).toEqual([1, 2, 5, 6]);
    });

    it("isCommitted is false when a head switch has not been pushed", () => {
        const m = dualHead();
        const committed = [1, 2, 3, 4]; // firmware still on head 0
        expect(isCommitted(m, setupFor(m), committed)).toBe(true);
        expect(isCommitted(m, engage(m, setupFor(m), 1), committed)).toBe(false);
    });

    it("treats an unknown or unread map as not committed, never as probably-fine", () => {
        const m = dualHead();
        expect(isCommitted(m, setupFor(m), null)).toBe(false);
        expect(isCommitted(m, setupFor(m), [1, 2, 3, 6])).toBe(false);
    });

    it("adoptCommitted takes the firmware's word on connect", () => {
        const m = dualHead(0);
        // The map survived in the Pico across a host reload: it is on head 1.
        expect(adoptCommitted(m, setupFor(m), [1, 2, 5, 6]).engaged).toBe(1);
    });

    it("adoptCommitted leaves the setup alone when the map means nothing", () => {
        const m = dualHead(0);
        const s = setupFor(m);
        expect(adoptCommitted(m, s, null)).toBe(s);
        expect(adoptCommitted(m, s, [null, null, null, null])).toBe(s);
    });
});

describe("sameSetup", () => {
    it("compares by value, not identity", () => {
        const m = dualHead();
        expect(sameSetup(setupFor(m), setupFor(m))).toBe(true);
    });

    it("notices a head switch and a re-mount", () => {
        const m = dualHead();
        const s = setupFor(m);
        expect(sameSetup(s, engage(m, s, 1))).toBe(false);
        expect(sameSetup(s, mount(m, s, 0, CREASE))).toBe(false);
    });
});
