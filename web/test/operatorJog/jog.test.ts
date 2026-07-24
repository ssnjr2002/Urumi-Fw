/**
 * Tests for operatorJog — click jog (open session blend), go-to-coordinate
 * jog (closed session + abort), over the in-process Sim.
 *
 * A background status poller runs during every test (mirrors Python's
 * _poll_worker daemon thread) so the ClickJogSource can pace against live
 * queuedUs / bufCount — without it an open session deadlocks because the
 * source cannot tell "machine still moving" from "undrained stall."
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../src/wire/link/link.js";
import { SimTransport } from "../../src/wire/link/backends/sim.js";
import { jogClick } from "../../src/operatorJog/jogClick.js";
import { jogTo } from "../../src/operatorJog/jogTo.js";
import { ClickJogSource } from "../../src/operatorJog/clickJogSource.js";
import { MachineState } from "../../src/wire/format/status.js";
import type { AxisCalibration } from "../../src/operatorJog/types.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    simOpts?: { frameMs?: number },
): Promise<T> {
    const sim = new SimTransport(simOpts);
    const link = new Link(sim);
    const poller = setInterval(() => {
        void link.getStatus().catch(() => undefined);
    }, 50);
    try {
        return await fn(link, sim);
    } finally {
        clearInterval(poller);
        await link.close();
    }
}

/** Poll until the Sim is IDLE with an empty ring, or `deadlineMs`. */
async function waitIdle(link: Link, deadlineMs = 2000): Promise<void> {
    const dl = Date.now() + deadlineMs;
    while (Date.now() < dl) {
        const st = await link.getStatus();
        if (st.state === MachineState.IDLE && st.bufCount === 0) return;
        await tick(20);
    }
}

const xAxis: AxisCalibration = { stepsPerUnit: 160, invert: false };

describe("operatorJog: jogClick (open session blend)", () => {
    it("one click completes and moves the axis", { timeout: 10000 }, async () => {
        await withLink(async (link) => {
            // 1 mm at 20 mm/s → 160 steps in ~0.08 s of motion
            const handle = jogClick(link, xAxis, "x", 1, 1, 20);
            const ok = await handle.done;
            expect(ok).toBe(true);

            await waitIdle(link, 3000);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.pos![0]).toBeGreaterThan(0);
        }, { frameMs: 5 });
    });

    it("abort mid-move discards the ring and lands IDLE", { timeout: 10000 }, async () => {
        await withLink(async (link) => {
            // a long-enough jog the executor won't finish before we abort
            const handle = jogClick(link, xAxis, "x", 1, 10, 2);
            await tick(80); // let a few packets land
            const stBefore = await link.getStatus();
            expect(stBefore.state).toBe(MachineState.RUNNING);

            handle.abort();
            expect(await handle.done).toBe(true);

            await waitIdle(link, 500);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.bufCount).toBe(0);
            expect(st.queuedUs).toBe(0);
        }, { frameMs: 5 });
    });

    it("cancel via ClickJogSource marks _cancelled, pull returns null", async () => {
        await withLink(async (link) => {
            const src = new ClickJogSource(xAxis, "x", 1, 10, link);
            src.add(1600);
            src.cancel();
            const batch = await src.pull({
                room: 16, inFlight: 0, emitted: 0, acked: 0,
            } as never);
            expect(batch).toBeNull();
        });
    });
});

describe("operatorJog: jogTo (closed absolute go-to)", () => {
    it("moves to the target; position advances", { timeout: 15000 }, async () => {
        await withLink(async (link) => {
            const prePos = (await link.getStatus()).pos![0];
            // 10 mm at 20 mm/s — faster to complete within timeout
            const handle = jogTo(link, xAxis, 0, 10, 20);
            const ok = await handle.done;
            expect(ok).toBe(true);

            await waitIdle(link, 5000);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.pos![0]).toBeGreaterThan(prePos);
        }, { frameMs: 5 });
    });

    it("abort() calls link.abort() mid-move", { timeout: 8000 }, async () => {
        await withLink(async (link) => {
            // enough distance that the executor hasn't finished before we abort,
            // but truncate() stops the session immediately so total time ≈ getStatus + one batch
            const handle = jogTo(link, xAxis, 0, 10, 2);
            await tick(80);
            handle.abort();
            const ok = await handle.done;
            expect(ok).toBe(true);

            await waitIdle(link, 500);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.bufCount).toBe(0);
        }, { frameMs: 5 });
    });

    it("already-at-target returns immediately true", async () => {
        await withLink(async (link) => {
            const handle = jogTo(link, xAxis, 0, 0, 10);
            expect(await handle.done).toBe(true);
        });
    });
});