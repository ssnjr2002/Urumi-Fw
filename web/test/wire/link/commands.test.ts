/**
 * Tests for wire/link/commands — the control-plane command helpers over a Link
 * backed by the in-process Sim. Exercises the live verbs (ping / getstate /
 * getpos / enable / disable / setorigin / pause / resume / cancel / stop /
 * unalarm / pingnode all / the proactive nodepos / vac_servo / vac_pump) and
 * the designed-for axisMap stub.
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
} from "../../../src/wire/link/commands.js";
import { MachineState } from "../../../src/wire/format/status.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
): Promise<T> {
    const sim = new SimTransport();
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
            expect(await pingNode(link, "all")).toBe(true);
            const all = await pingAll(link);
            expect(all.get(1)).toBe(true);
            expect(all.get(4)).toBe(true);
            expect(all.size).toBe(4);
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
    it("nodepos: the sim returns err unknown → throws", async () => {
        await withLink(async (link) => {
            // The sim does not implement `nodepos` (it's a CLI-only verb the
            // firmware has but the Python SimBackend predates). The reply is
            // "err unknown", which fails the `node <id> pos <steps>` pattern.
            await expect(nodePos(link, 3)).rejects.toThrow(/bad nodepos reply/);
        });
    });

    it("vacServo / vacPump: sim returns err unknown → false", async () => {
        await withLink(async (link) => {
            expect(await vacServo(link, 5, 1, true)).toBe(false);
            expect(await vacPump(link, 5, true)).toBe(false);
        });
    });
});

describe("wire/link/commands: designed-for axisMap stub", () => {
    it("throws a not-implemented error with the doc reference", async () => {
        await withLink(async (link) => {
            await expect(axisMap(link, 1, 2, 3, 4)).rejects.toThrow(/not yet implemented/);
            await expect(axisMap(link, 1, 2, 3, 4)).rejects.toThrow(/engage_and_axis_map/);
        });
    });
});