/**
 * controller.test.ts — the three invariants, against the Sim.
 *
 * Deliberately an integration test over a real Link and a real SimTransport
 * rather than a mock. The Controller exists precisely because the join between
 * config and transport is where the mistakes live, and a mocked Link would let
 * every one of them through — the Sim boots into ALARM_CONFIG with no axis map,
 * exactly like the firmware, which is what makes the reconciliation tests mean
 * anything.
 */

import { describe, it, expect } from "vitest";
import { Controller, BusyError } from "../../src/controller/controller.js";
import { Link } from "../../src/wire/link/link.js";
import { SimTransport } from "../../src/wire/link/backends/sim.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    ToolType,
} from "../../src/machine/schema.js";
import { KNIFE, PEN } from "../../src/machine/tools.js";
import { resolvedAxesDefault } from "../../src/machine/resolve.js";
import { MachineState } from "../../src/wire/format/status.js";
import { setOrigin } from "../../src/wire/link/commands.js";

/** Head 0: knife, Z at 1200 steps/mm on node 3. Head 1: pen, Z at 600 on node 5. */
function dualHead(defaultHead = 0) {
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        [
            toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667, { rotary: true }), {
                accepts: [ToolType.KNIFE],
                xOffset: 0,
            }),
            toolHead(axisConfig(busNode(5), 600), axisConfig(busNode(6), 51.667, { rotary: true }), {
                accepts: [ToolType.PEN],
                xOffset: 50,
                yOffset: 10,
            }),
        ],
        { defaultHead },
    );
}

function bench(opts: { axisMap?: readonly (number | null)[] } = {}) {
    const machine = dualHead();
    const sim = new SimTransport({ busNodes: [1, 2, 3, 4, 5, 6], frameMs: 5, ...opts });
    const link = new Link(sim);
    return { machine, sim, link, controller: new Controller(machine, link, { pollMs: 5 }) };
}

// ── invariant A: exclusivity ─────────────────────────────────────────────────

describe("exclusivity", () => {
    it("refuses a second claim and names what holds it", () => {
        const { controller } = bench();
        const held = controller.acquire("job");
        expect(controller.busy).toBe("job");
        expect(() => controller.acquire("jog")).toThrow(BusyError);
        try {
            controller.acquire("jog");
        } catch (e) {
            // "busy" alone sends the operator hunting for a phantom.
            expect((e as BusyError).holder).toBe("job");
        }
        held.release();
        expect(controller.busy).toBeNull();
        expect(held.active).toBe(false);
    });

    it("releases the lease even when the work throws", async () => {
        const { controller } = bench();
        await expect(
            controller.withLease("job", () => Promise.reject(new Error("stream died"))),
        ).rejects.toThrow("stream died");
        expect(controller.busy).toBeNull();
        // ...and the machine is usable again, which is the point.
        expect(() => controller.acquire("jog")).not.toThrow();
    });

    it("tolerates a double release rather than corrupting the holder", () => {
        const { controller } = bench();
        const a = controller.acquire("jog");
        a.release();
        const b = controller.acquire("job");
        a.release(); // stale handle from the finished jog
        expect(controller.busy).toBe("job");
        b.release();
    });

    it("lets estop and abort through while something holds the lease", async () => {
        const { controller, sim } = bench();
        controller.acquire("job");
        // An estop that waits its turn is not an estop.
        await expect(controller.estop()).resolves.toBeUndefined();
        expect(() => controller.abort()).not.toThrow();
        expect(sim.state).toBe(MachineState.ALARM);
    });
});

// ── invariant B: setup reconciliation ────────────────────────────────────────

describe("setup reconciliation", () => {
    it("reports an unbound machine as not synced, never as probably-fine", async () => {
        const { controller } = bench();
        expect(controller.synced).toBe(false); // nothing read yet
        await controller.sync();
        expect(controller.committed).toEqual([null, null, null, null]);
        expect(controller.synced).toBe(false);
    });

    it("commit binds the engaged head's slots and reads them back", async () => {
        const { controller } = bench();
        await controller.commit(0);
        expect(controller.committed).toEqual([1, 2, 3, 4]);
        expect(controller.synced).toBe(true);

        await controller.commit(1);
        expect(controller.committed).toEqual([1, 2, 5, 6]);
        expect(controller.setup.engaged).toBe(1);
        expect(controller.synced).toBe(true);
    });

    it("refreshes status after a commit — a rebind changes it without moving", async () => {
        // The firmware re-derives machinePos / axes_homed / axes_enabled for
        // every slot it binds, adopting the incoming node's own state. So the
        // host's last sample describes the OUTGOING head the instant a commit
        // lands, and without a refresh the UI shows a stale homed mask until the
        // background poll catches up — indefinitely, if auto-poll is off.
        const { controller } = bench();
        let samples = 0;
        controller.on("status", () => samples++);

        await controller.commit(0);
        expect(samples).toBeGreaterThan(0);

        const before = samples;
        await controller.commit(1);
        expect(samples).toBeGreaterThan(before);
    });

    it("a datum survives a head swap — swapping heads is not re-homing", async () => {
        // The datum lives with the NODE (core0/position.cpp), so head 0's Z on
        // node 3 keeps it while slot 2 is lent to node 5 and gets it back on the
        // return trip. The mask is re-derived per bind, which is what makes the
        // middle assertion the interesting one: node 5 has never been datumed,
        // and a slot-framed mask would have reported it as homed.
        const { controller, link, sim } = bench();
        await controller.commit(0);              // slots = 1 2 3 4
        await setOrigin(link, "z");
        await controller.refresh();
        expect(controller.status!.homed("z")).toBe(true);

        await controller.commit(1);              // slot 2 -> node 5
        expect(controller.status!.homed("z")).toBe(false);

        await controller.commit(0);              // slot 2 -> node 3 again
        expect(controller.status!.homed("z")).toBe(true);
        expect(sim.nodeHomed.has(3)).toBe(true);
    });

    it("sync adopts the head the firmware is actually bound to", async () => {
        // The map survives in the Pico across a host reload: a fresh page has no
        // idea which head is engaged, and guessing defaultHead then fighting the
        // machine is worse than asking.
        const { controller, machine, link } = bench();
        await controller.commit(1);

        const fresh = new Controller(machine, link);
        expect(fresh.setup.engaged).toBe(0); // defaultHead, the guess
        expect(await fresh.sync()).toBe(true);
        expect(fresh.setup.engaged).toBe(1); // the truth
    });

    it("engage without commit leaves the machine owing a commit", async () => {
        const { controller } = bench();
        await controller.commit(0);
        controller.engage(1);
        expect(controller.setup.engaged).toBe(1);
        expect(controller.synced).toBe(false); // pending, not committed
    });

    it("refuses to rebind slots while RUNNING", async () => {
        const { controller, sim } = bench();
        await controller.commit(0);
        sim.state = MachineState.RUNNING;
        await controller.refresh();
        await expect(controller.commit(1)).rejects.toThrow(/while RUNNING/);
        // and the host-side setup did not move either
        expect(controller.setup.engaged).toBe(0);
    });

    it("records an unreadable map as null rather than keeping the last good one", async () => {
        // A port that has gone away answers nothing, and `readAxisMap` rejects.
        // Keeping the last good value here would be the worst possible answer:
        // it reads as "still bound" for a machine nobody can see.
        const machine = dualHead();
        let alive = true;
        const link = {
            closed: false,
            command: (text: string) =>
                Promise.resolve(alive && text === "axis_map" ? "axis_map 1 2 3 4" : ""),
        } as unknown as Link;
        const controller = new Controller(machine, link);

        await controller.readCommitted();
        expect(controller.committed).toEqual([1, 2, 3, 4]);
        expect(controller.synced).toBe(true);

        alive = false;
        await controller.readCommitted();
        expect(controller.committed).toBeNull();
        expect(controller.synced).toBe(false);
    });
});

// ── invariant C: frame-correct live position ─────────────────────────────────

describe("live position", () => {
    it("reads Z through the ENGAGED head, not the default one", async () => {
        const { controller, sim } = bench();
        sim.pos = [0, 0, 1200, 0]; // 1 mm on head 0's leadscrew, 2 mm on head 1's
        await controller.refresh();

        expect(controller.axisUnits("z")).toBeCloseTo(1.0, 9);
        controller.engage(1);
        expect(controller.axisUnits("z")).toBeCloseTo(2.0, 9);

        // The bug this prevents, stated: the static answer never moves.
        expect(resolvedAxesDefault(controller.machine).z.stepsPerUnit).toBe(1200);
    });

    it("separates head centre from tool tip", async () => {
        const { controller, sim } = bench();
        sim.pos = [1600, 1600, 0, 0]; // 10 mm, 10 mm
        await controller.refresh();

        expect(controller.homeXY()).toEqual({ x: 10, y: 10 });
        controller.engage(1); // pen head, offset (50, 10)
        expect(controller.headXY()).toEqual({ x: 60, y: 20 });

        const tip = controller.tipXY()!;
        // The two frames differ by exactly the tool offset — which is why a
        // caller must not be able to land in the wrong one by omission.
        expect(tip.x - 60).toBeCloseTo(PEN.toolOffset.xOffset, 9);
        expect(tip.y - 20).toBeCloseTo(PEN.toolOffset.yOffset, 9);
    });

    it("throws for a tip on an empty socket instead of reporting head centre", async () => {
        const { controller, sim } = bench();
        sim.pos = [0, 0, 0, 0];
        await controller.refresh();
        controller.mount(0, null);
        expect(() => controller.tipXY()).toThrow(/no tool fitted/);
        expect(controller.headXY()).toEqual({ x: 0, y: 0 });
    });

    it("returns null rather than the origin when no sample carries a position", () => {
        const { controller } = bench();
        expect(controller.homeXY()).toBeNull();
        expect(controller.axisUnits("x")).toBeNull();
    });
});

// ── observation ──────────────────────────────────────────────────────────────

describe("events", () => {
    it("emits status, setup, committed and busy", async () => {
        const { controller } = bench();
        const seen: string[] = [];
        controller.on("status", () => seen.push("status"));
        controller.on("setup", () => seen.push("setup"));
        controller.on("committed", () => seen.push("committed"));
        controller.on("busy", (b) => seen.push(`busy:${b ?? "-"}`));

        await controller.refresh();
        await controller.commit(1);
        controller.acquire("jog").release();

        expect(seen).toContain("status");
        expect(seen).toContain("setup");
        expect(seen).toContain("committed");
        expect(seen).toEqual(expect.arrayContaining(["busy:jog", "busy:-"]));
    });

    it("unsubscribes, and one throwing subscriber does not sink the others", async () => {
        const { controller } = bench();
        let a = 0;
        let b = 0;
        const off = controller.on("status", () => {
            a++;
        });
        controller.on("status", () => {
            throw new Error("render bug");
        });
        controller.on("status", () => {
            b++;
        });

        await expect(controller.refresh()).resolves.toBeDefined();
        expect([a, b]).toEqual([1, 1]);
        off();
        await controller.refresh();
        expect([a, b]).toEqual([1, 2]);
    });

    it("polls in the background and stops cleanly", async () => {
        const { controller } = bench();
        let n = 0;
        controller.on("status", () => {
            n++;
        });
        controller.startPolling(1);
        await new Promise((r) => setTimeout(r, 40));
        await controller.stopPolling();
        const settled = n;
        expect(settled).toBeGreaterThan(1);
        await new Promise((r) => setTimeout(r, 20));
        expect(n).toBe(settled); // no stray poll after stop
    });

    it("surfaces a poll failure as an error event instead of an unhandled rejection", async () => {
        const link = {
            closed: false,
            getStatus: () => Promise.reject(new Error("port went away")),
        } as unknown as Link;
        const controller = new Controller(dualHead(), link);
        const errors: Error[] = [];
        controller.on("error", (e) => errors.push(e));
        controller.startPolling(1);
        await new Promise((r) => setTimeout(r, 30));
        await controller.stopPolling();
        expect(errors.length).toBeGreaterThan(0);
    });
});

describe("close", () => {
    it("forgets the committed map but keeps the setup", async () => {
        const { controller } = bench();
        await controller.commit(1);
        await controller.close();
        // What is screwed into the machine did not change because we hung up.
        expect(controller.setup.engaged).toBe(1);
        // What the firmware is bound to, we can no longer see.
        expect(controller.committed).toBeNull();
        expect(controller.synced).toBe(false);
    });
});
