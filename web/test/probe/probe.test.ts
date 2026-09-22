/**
 * probe.test.ts — heights, the probe leg plan, the getstate probe fields, and
 * prepareZ / runWalk against the sim's setprobe/unprobe model.
 */

import { describe, it, expect } from "vitest";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    ToolType,
    NodeType,
    KNIFE,
    PEN,
    type ProbeConfig,
    type MachineConfig,
} from "../../src/machine/index.js";
import { toolHeights, zAtHeightSteps } from "../../src/machine/heights.js";
import { deriveProbePlan } from "../../src/probe/derive.js";
import { parseGetstate } from "../../src/wire/format/status.js";
import { Controller } from "../../src/controller/controller.js";
import { prepareZ, touchOffHere } from "../../src/controller/prepareZ.js";
import { jogToPoint } from "../../src/operatorJog/jogTo.js";
import { runWalk } from "../../src/controller/runWalk.js";
import { Link } from "../../src/wire/link/link.js";
import { SimTransport } from "../../src/wire/link/backends/sim.js";
import type { Mounts } from "../../src/production/schedule.js";

const PROBE: ProbeConfig = {
    node: busNode(7, { type: NodeType.VACUUM }),
    switchXMm: 10,
    switchYMm: 20,
    replyDeadlineUs: 2000,
    probeTravel: 30,
    pullInFeed: 2.5,
    seekFeed: 6,
    latchFeed: 0.78,
    rampSteps: 400,
    backoffMm: 1,
    parkMm: 2,
    seekOvertravelMm: 0.05,
    tripMm: 0.5,
};

function machine(zInvert = false, probe: ProbeConfig | null = PROBE): MachineConfig {
    const z = (id: number) => axisConfig(busNode(id), 1200, {
        invert: zInvert, maxFeed: 20, maxAccel: 500, ...(probe ? { probe } : {}),
        homing: {
            kind: "linear", hardTravel: 40, atOrigin: true, pullInFeed: 2.5,
            seekFeed: 6, latchFeed: 0.78, rampSteps: 400, backoffMm: 4, parkMm: 4,
        },
    });
    return machineConfig(
        axisConfig(busNode(1), 160, { maxFeed: 80, maxAccel: 1000 }),
        axisConfig(busNode(2), 160, { maxFeed: 80, maxAccel: 1000 }),
        [
            toolHead(z(3), axisConfig(busNode(4), 51.667, { rotary: true }), {
                accepts: [ToolType.KNIFE],
            }),
            toolHead(z(5), axisConfig(busNode(6), 51.667, { rotary: true }), {
                accepts: [ToolType.PEN],
                xOffset: 50,
            }),
        ],
        { rapid: { feed: 80 }, z: { feed: 10 }, clearanceMm: 2 },
    );
}

describe("toolHeights", () => {
    it("a plunging tool cuts at the mat, a surface tool at the material top", () => {
        const m = machine();
        expect(toolHeights(KNIFE, m, 3)).toEqual({ cutMm: 0, clearMm: 5, liftMm: 5 });
        expect(toolHeights(PEN, m, 3)).toEqual({ cutMm: 3, clearMm: 5, liftMm: 2 });
    });

    it("refuses a negative or non-numeric material thickness", () => {
        expect(() => toolHeights(PEN, machine(), -1)).toThrow(/materialMm/);
        expect(() => toolHeights(PEN, machine(), NaN)).toThrow(/materialMm/);
    });
});

describe("zAtHeightSteps", () => {
    it("measures up from the mat, which is the contact lowered by tripMm", () => {
        const z = machine().heads[0]!.z;
        // mat = 40000 + 0.5 mm down; 5 mm above it is 4.5 mm above the contact.
        expect(zAtHeightSteps(40000, 5, z)).toBe(40000 - 4.5 * 1200);
    });

    it("flips with invert", () => {
        const z = machine(true).heads[0]!.z;
        expect(zAtHeightSteps(-40000, 5, z)).toBe(-40000 + 4.5 * 1200);
    });

    it("a Z with no probe block stores the mat itself", () => {
        const { probe: _, ...z } = machine().heads[0]!.z;
        expect(zAtHeightSteps(40000, 0, z)).toBe(40000);
    });
});

describe("deriveProbePlan", () => {
    it("seeks down, backs off, latches down one step per poll, parks up", () => {
        const plan = deriveProbePlan(machine().heads[0]!.z);
        expect(plan.switchNode).toBe(7);
        expect(plan.legs.map((l) => [l.name, l.args.dir, l.args.retract])).toEqual([
            ["seek", 1, false],
            ["backoff", 0, true],
            ["latch", 1, false],
            ["park", 0, true],
        ]);
        const [seek, backoff, latch, park] = plan.legs.map((l) => l.args);
        expect(seek!.pollDiv).toBe(60);            // 0.05 mm × 1200
        expect(seek!.maxSteps).toBe(36000);        // 30 mm
        expect(latch!.pollDiv).toBe(1);
        expect(latch!.ceilUs).toBe(Math.round(1e6 / (0.78 * 1200)));
        expect(latch!.rampSteps).toBe(0);
        expect(latch!.maxSteps).toBe(3000);        // 2.5 × backoff
        expect(backoff!.maxSteps).toBe(1200);
        expect(park!.maxSteps).toBe(2400);
    });

    it("down is dir 0 on an inverted Z", () => {
        const plan = deriveProbePlan(machine(true).heads[0]!.z);
        expect(plan.legs[0]!.args.dir).toBe(0);
    });

    it("refuses a Z with no probe block", () => {
        expect(() => deriveProbePlan(axisConfig(busNode(3), 1200))).toThrow(/probe/);
    });
});

describe("getstate probe fields", () => {
    const base = "state=6 enabled=0x0f homed=0x0f alarm=0 running=0 latched=0x00";

    it("reads the stored contact height", () => {
        expect(parseGetstate(`${base} probed=1 pz=47150`).probeZ).toBe(47150);
    });

    it("probed=0 is null, an absent field is undefined", () => {
        expect(parseGetstate(`${base} probed=0`).probeZ).toBeNull();
        expect(parseGetstate(base).probeZ).toBeUndefined();
    });

    it("reads the session phase and last cause", () => {
        const st = parseGetstate(`${base} probing=2 probe=0 retries=0 psteps=12 probed=0`);
        expect(st.probing).toBe(2);
        expect(st.probeCause).toBe(0);
    });
});

async function bench(m = machine()) {
    const sim = new SimTransport({ busNodes: [1, 2, 3, 4, 5, 6, 7], frameMs: 2, ringSize: 256 });
    const link = new Link(sim);
    const controller = new Controller(m, link, { pollMs: 2 });
    await controller.commit(0);
    expect(await link.command("setorigin xyz")).toBe("ok");
    return { m, sim, link, controller };
}

describe("prepareZ", () => {
    it("moves an already-probed Z to clear height", async () => {
        const { m, sim, link, controller } = await bench();
        // Contact just below where Z stands, so the move to clear is short.
        expect(await link.command("setprobe 6000")).toMatch(/^ok probe node=3/);

        const clear = await prepareZ(controller, m, 0, KNIFE, 3);
        expect(clear).toBe(zAtHeightSteps(6000, 5, m.heads[0]!.z));
        expect(sim.pos[2]).toBe(clear);
        await link.close();
    });

    it("without a probe block, takes the operator's touch-off and returns XY", async () => {
        const { m, sim, link, controller } = await bench(machine(false, null));
        let asked = 0;
        const clear = await prepareZ(controller, m, 0, PEN, 1, {
            touchOff: async () => {
                asked++;
                // The operator wanders off in X, touches the mat where Z stands.
                expect(await link.resetSeq()).toBe(true);
                expect(await jogToPoint(link, [{ axisIndex: 0, axis: m.x, targetPos: 2 }], 20).done).toBe(true);
                expect(await controller.waitAtRest()).toBe(true);
                expect((await touchOffHere(link)).ok).toBe(true);
            },
        });
        expect(asked).toBe(1);
        expect(await link.command("getstate")).toMatch(/probed=1 pz=0$/);
        // clear = 1 mm material + 2 mm clearance above the mat.
        expect(clear).toBe(-3 * 1200);
        expect(sim.pos[2]).toBe(clear);
        expect(sim.pos[0]).toBe(0);
        await link.close();
    }, 20000);

    it("refuses a touch-off that stored nothing", async () => {
        const { m, link, controller } = await bench(machine(false, null));
        await expect(prepareZ(controller, m, 0, PEN, 1, { touchOff: async () => {} }))
            .rejects.toThrow(/holds no height/);
        await link.close();
    });

    it("without a probe block or a touch-off handler, refuses", async () => {
        const { m, link, controller } = await bench(machine(false, null));
        await expect(prepareZ(controller, m, 0, PEN, 1)).rejects.toThrow(/touch-off/);
        await link.close();
    });
});

describe("runWalk invalidates the probe on a swap", () => {
    it("unprobes a head whose tool changed", async () => {
        const { link, controller } = await bench();
        expect(await link.command("setprobe 30000")).toMatch(/^ok/);

        const mounts: Mounts = [ToolType.KNIFE, ToolType.PEN];
        await runWalk(controller, [{ kind: "pause", swapIn: [ToolType.KNIFE], swapOut: [], mounts }], 0, {
            prepareZ: async () => {},
            // A fresh knife in head 0: same type, different tool.
            confirmSwap: () => (controller.mount(0, { ...KNIFE }), true),
        });

        expect(await link.command("getstate")).toMatch(/probed=0/);
        await link.close();
    });

    it("keeps the probe when no tool changed", async () => {
        const { link, controller } = await bench();
        expect(await link.command("setprobe 30000")).toMatch(/^ok/);

        const mounts: Mounts = [ToolType.KNIFE, ToolType.PEN];
        await runWalk(controller, [{ kind: "pause", swapIn: [], swapOut: [], mounts }], 0, {
            prepareZ: async () => {},
            confirmSwap: () => true,
        });

        expect(await link.command("getstate")).toMatch(/probed=1 pz=30000/);
        await link.close();
    });
});
