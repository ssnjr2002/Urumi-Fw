/**
 * Tests for config helpers — layer→tool resolution and the bake-time
 * node-presence feasibility gate.
 */

import { describe, it, expect } from "vitest";
import { toolForLayer, canRunTool, requiredAxes } from "./helpers.js";
import {
    PEN,
    KNIFE,
    CREASE,
    REVOLVER_PEN,
    axisConfig,
    busNode,
    toolHead,
    toolProfile,
    machineConfig,
    ToolType,
    type MachineConfig,
} from "./config.js";

// A machine with every axis node wired; per-test we knock out nodes.
function machine(opts?: {
    xPresent?: boolean;
    yPresent?: boolean;
    zPresent?: boolean;
    aPresent?: boolean;
    peripherals?: { role: string; present: boolean }[];
}): MachineConfig {
    const {
        xPresent = true,
        yPresent = true,
        zPresent = true,
        aPresent = true,
        peripherals = [],
    } = opts ?? {};
    const head = toolHead(
        axisConfig(busNode(3, { present: zPresent }), 1200),
        axisConfig(busNode(4, { present: aPresent }), 51.667, { rotary: true }),
    );
    return machineConfig(
        axisConfig(busNode(1, { present: xPresent }), 160),
        axisConfig(busNode(2, { present: yPresent }), 160),
        [head],
        {
            peripherals: peripherals.map((p, i) =>
                busNode(10 + i, { role: p.role, present: p.present }),
            ),
        },
    );
}

describe("toolForLayer", () => {
    it("maps a case-insensitive name to its profile", () => {
        expect(toolForLayer("knife")).toBe(KNIFE);
        expect(toolForLayer("Knife")).toBe(KNIFE);
        expect(toolForLayer("  PEN ")).toBe(PEN);
    });

    it("returns undefined for an unknown name", () => {
        expect(toolForLayer("laser")).toBeUndefined();
    });
});

describe("requiredAxes", () => {
    it("pen with no lift needs neither Z nor A", () => {
        expect(requiredAxes(PEN)).toEqual({ z: false, a: false });
    });

    it("tangential tools need A", () => {
        expect(requiredAxes(KNIFE).a).toBe(true);
        expect(requiredAxes(CREASE).a).toBe(true);
    });

    it("revolver needs A (slot selection) though not tangential", () => {
        expect(REVOLVER_PEN.tangential).toBe(false);
        expect(requiredAxes(REVOLVER_PEN).a).toBe(true);
    });

    it("Z is required only when the tool lifts", () => {
        const lifter = toolProfile("lifter", { liftHeight: 2 });
        expect(requiredAxes(lifter).z).toBe(true);
        expect(requiredAxes(PEN).z).toBe(false);
    });
});

describe("canRunTool (node presence, not mount)", () => {
    it("pen runs on a bus with only X/Y present", () => {
        const [ok, reason] = canRunTool(machine({ zPresent: false, aPresent: false }), PEN);
        expect(ok).toBe(true);
        expect(reason).toBe("");
    });

    it("does NOT depend on what tool is mounted on the head", () => {
        // head seeds PEN, but a KNIFE runs because the A node is wired
        const [ok] = canRunTool(machine(), KNIFE);
        expect(ok).toBe(true);
    });

    it("knife fails when no head has an A node wired", () => {
        const [ok, reason] = canRunTool(machine({ aPresent: false }), KNIFE);
        expect(ok).toBe(false);
        expect(reason).toContain("A axis node");
    });

    it("fails when X node is not present", () => {
        const [ok, reason] = canRunTool(machine({ xPresent: false }), PEN);
        expect(ok).toBe(false);
        expect(reason).toContain("X axis node");
    });

    it("a lifting tool fails when no Z node is wired", () => {
        const lifter = toolProfile("lifter", { liftHeight: 2 });
        const [ok, reason] = canRunTool(machine({ zPresent: false }), lifter);
        expect(ok).toBe(false);
        expect(reason).toContain("Z axis node");
    });

    it("requires a present peripheral for each required role", () => {
        const suction = toolProfile("suction", {
            toolType: ToolType.PEN,
            requiredPeripheralRoles: ["vacuum"],
        });
        expect(canRunTool(machine({ peripherals: [] }), suction)[0]).toBe(false);
        expect(
            canRunTool(machine({ peripherals: [{ role: "vacuum", present: false }] }), suction)[0],
        ).toBe(false);
        expect(
            canRunTool(machine({ peripherals: [{ role: "vacuum", present: true }] }), suction)[0],
        ).toBe(true);
    });

    it("lists every missing node in one reason", () => {
        const [ok, reason] = canRunTool(machine({ xPresent: false, aPresent: false }), KNIFE);
        expect(ok).toBe(false);
        expect(reason).toContain("X axis node");
        expect(reason).toContain("A axis node");
    });
});
