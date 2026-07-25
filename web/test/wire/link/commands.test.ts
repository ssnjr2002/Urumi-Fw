/**
 * Tests for wire/link/commands — the control-plane command helpers over a Link
 * backed by the in-process Sim. Exercises the live verbs (ping / getstate /
 * getpos / enable / disable / setorigin / pause / resume / cancel / stop /
 * unalarm / pingnode all / the proactive nodepos / vac_servo / vac_pump) and
 * axis_map with the ALARM_CONFIG boot gate it clears.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../src/wire/link/link.js";
import { SimTransport } from "../../../src/wire/link/backends/sim.js";
import {
    ping,
    pingNode,
    pingAll,
    getState,
    getStatus,
    getPos,
    nodePos,
    vacServo,
    vacPump,
    enable,
    disable,
    setOrigin,
    pause,
    resume,
    cancel,
    stop,
    unalarm,
    axisMap,
    readAxisMap,
} from "../../../src/wire/link/commands.js";
import { MachineState, AlarmReason } from "../../../src/wire/format/status.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
): Promise<T> {
    // Configured boot; the axis_map/gate behaviour has its own describe block.
    const sim = new SimTransport({ axisMap: [1, 2, 3, 4] });
    const link = new Link(sim);
    try {
        return await fn(link, sim);
    } finally {
        await link.close();
    }
}

describe("wire/link/commands: liveness + state", () => {
    it("ping → true", async () => {
        await withLink(async (link) => {
            expect(await ping(link)).toBe(true);
        });
    });

    it("pingNode single → true; pingAll returns all nodes true", async () => {
        await withLink(async (link) => {
            expect(await pingNode(link, 3)).toBe(true);
            // "all" is every-node-answered, and the scan covers the whole bus —
            // the default sim has nodes 1..4, so ids 5..8 time out and the
            // aggregate verdict is false.
            expect(await pingNode(link, "all")).toBe(false);
            const all = await pingAll(link);
            expect(all.get(1)).toBe(true);
            expect(all.get(4)).toBe(true);
            expect(all.get(5)).toBe(false);
            expect(all.size).toBe(8);
        });
    });

    it("getState returns a parsed MachineStatus", async () => {
        await withLink(async (link) => {
            const st = await getState(link);
            expect(st.state).toBe(MachineState.IDLE);
        });
    });

    it("getStatus (binary) returns parsed MachineStatus with binary-only fields", async () => {
        await withLink(async (link) => {
            const st = await getStatus(link);
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.bufCount).toBe(0);
            expect(st.pos).not.toBeUndefined();
        });
    });

    it("getPos returns [x, y, z, a]", async () => {
        await withLink(async (link) => {
            expect(await getPos(link)).toEqual([0, 0, 0, 0]);
        });
    });
});

describe("wire/link/commands: enable / disable / setorigin / unalarm", () => {
    it("enable all → true; disable all → true; setorigin → true", async () => {
        await withLink(async (link) => {
            expect(await enable(link)).toBe(true);
            expect(await disable(link)).toBe(true);
            expect(await setOrigin(link)).toBe(true);
        });
    });

    it("per-node enable / disable", async () => {
        await withLink(async (link) => {
            expect(await enable(link, 2)).toBe(true);
            expect(await disable(link, 2)).toBe(true);
        });
    });

    it("stop → state=ALARM; unalarm → IDLE", async () => {
        await withLink(async (link) => {
            await stop(link);
            await tick(20);
            expect((await getStatus(link)).state).toBe(MachineState.ALARM);
            expect(await unalarm(link)).toBe(true);
            expect((await getStatus(link)).state).toBe(MachineState.IDLE);
        });
    });
});

describe("wire/link/commands: pause / resume / cancel", () => {
    it("pause rejected in IDLE; after forceRunning, pause → PAUSED, resume → IDLE", async () => {
        await withLink(async (link, sim) => {
            expect(await pause(link)).toBe(false); // IDLE
            sim._forceRunning();
            expect(await pause(link)).toBe(true);
            expect((await getStatus(link)).state).toBe(MachineState.PAUSED);
            expect(await resume(link)).toBe(true);
            expect((await getStatus(link)).state).toBe(MachineState.IDLE);
        });
    });

    it("cancel in PAUSED → IDLE", async () => {
        await withLink(async (link, sim) => {
            sim._forceRunning();
            await pause(link);
            expect(await cancel(link)).toBe(true);
            expect((await getStatus(link)).state).toBe(MachineState.IDLE);
        });
    });
});

describe("wire/link/commands: proactive verbs (vacuum + nodepos)", () => {
    it("nodepos: reports the bound slot's position", async () => {
        await withLink(async (link) => {
            // The sim has no per-node counter distinct from machinePos, so it
            // reports the slot's position — the no-lost-steps case, which is
            // the only one a sim can model. Node 3 is bound to slot 2 (Z).
            expect(await nodePos(link, 3)).toEqual({ nodeId: 3, pos: 0 });
        });
    });

    it("nodepos: an id with no node on the bus times out → throws", async () => {
        await withLink(async (link) => {
            await expect(nodePos(link, 7)).rejects.toThrow(/bad nodepos reply/);
        });
    });

    it("vacServo / vacPump: sim returns err unknown → false", async () => {
        await withLink(async (link) => {
            expect(await vacServo(link, 5, 1, true)).toBe(false);
            expect(await vacPump(link, 5, true)).toBe(false);
        });
    });
});

describe("wire/link/commands: axis_map + the ALARM_CONFIG gate", () => {
    /** An UNCONFIGURED sim — the firmware's real boot state. */
    async function withUnconfigured<T>(fn: (link: Link, sim: SimTransport) => Promise<T>): Promise<T> {
        const sim = new SimTransport({ busNodes: [1, 2, 3, 4, 5, 6] });
        const link = new Link(sim);
        try {
            return await fn(link, sim);
        } finally {
            await link.close();
        }
    }

    it("boots into ALARM/ALARM_CONFIG with an empty map", async () => {
        await withUnconfigured(async (link) => {
            const st = await getStatus(link);
            expect(st.state).toBe(MachineState.ALARM);
            expect(st.alarm).toBe(AlarmReason.CONFIG);
            expect(await readAxisMap(link)).toEqual([null, null, null, null]);
        });
    });

    it("committing a map clears the gate → IDLE", async () => {
        await withUnconfigured(async (link) => {
            expect(await axisMap(link, 1, 2, 3, 4)).toBe(true);
            const st = await getStatus(link);
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.alarm).toBe(AlarmReason.NONE);
            expect(await readAxisMap(link)).toEqual([1, 2, 3, 4]);
        });
    });

    it("unalarm cannot clear the config gate", async () => {
        await withUnconfigured(async (link) => {
            expect(await link.command("unalarm")).toBe("err unconfigured");
            expect((await getStatus(link)).state).toBe(MachineState.ALARM);
        });
    });

    it("setorigin does not clear the config gate either", async () => {
        await withUnconfigured(async (link) => {
            expect(await setOrigin(link)).toBe(true); // the datum IS set
            expect((await getStatus(link)).state).toBe(MachineState.ALARM);
        });
    });

    it("a node that does not answer leaves the previous map committed", async () => {
        await withUnconfigured(async (link) => {
            await axisMap(link, 1, 2, 3, 4);
            // 7 is not on this sim's bus — the engage times out.
            await expect(axisMap(link, 1, 2, 7, 4)).rejects.toThrow(/node 7 timeout/);
            expect(await readAxisMap(link)).toEqual([1, 2, 3, 4]);
        });
    });

    it("rejects a duplicate binding and an out-of-range id", async () => {
        await withUnconfigured(async (link) => {
            await expect(axisMap(link, 1, 1, 3, 4)).rejects.toThrow(/dup/);
            await expect(axisMap(link, 1, 2, 3, 99)).rejects.toThrow(/bad_node/);
        });
    });

    it("null/0 bindings leave a slot disengaged (single-head machine)", async () => {
        await withUnconfigured(async (link) => {
            expect(await axisMap(link, 1, 2, 3, null)).toBe(true);
            expect(await readAxisMap(link)).toEqual([1, 2, 3, null]);
        });
    });

    it("a head switch rebinds slots 2 and 3 only", async () => {
        await withUnconfigured(async (link) => {
            await axisMap(link, 1, 2, 3, 4); // head A
            expect(await axisMap(link, 1, 2, 5, 6)).toBe(true); // head B
            expect(await readAxisMap(link)).toEqual([1, 2, 5, 6]);
        });
    });

    it("enable keys the axes_enabled bits on SLOT, not node id", async () => {
        await withUnconfigured(async (link) => {
            // Head B on slots 2/3: nodes 5 and 6. Under the old `id - 1`
            // bookkeeping, enabling node 5 would have set bit 4 — off the end
            // of the 4-slot mask.
            await axisMap(link, 1, 2, 5, 6);
            expect(await enable(link, 5)).toBe(true);
            expect((await getStatus(link)).axesEnabled).toBe(0b0100); // slot 2
        });
    });

    it("an unmapped node relays but touches no axis state", async () => {
        await withUnconfigured(async (link) => {
            await axisMap(link, 1, 2, 3, 4);
            expect(await enable(link, 6)).toBe(true); // e.g. a vacuum node
            expect((await getStatus(link)).axesEnabled).toBe(0);
        });
    });
});