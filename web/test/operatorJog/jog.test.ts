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
import { jogTo, jogToPoint } from "../../src/operatorJog/jogTo.js";
import { ClickJogSource } from "../../src/operatorJog/clickJogSource.js";
import { MachineState } from "../../src/wire/format/status.js";
import type { AxisCalibration } from "../../src/operatorJog/types.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    simOpts?: { frameMs?: number },
): Promise<T> {
    // Configured boot — the ALARM_CONFIG gate would refuse every jog.
    const sim = new SimTransport({ axisMap: [1, 2, 3, 4], ...simOpts });
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

    // Locks the wire-frame delta formula: invert=true → wire delta is
    // coordinate delta × -1. target +10mm → wireTarget = -1600 steps.
    // machinePos tracks wire steps, so after idle pos[0] ≈ -1600.
    it("invert=true: wire-frame delta is negative for a positive coordinate target", { timeout: 15000 }, async () => {
        const invAxis: AxisCalibration = { stepsPerUnit: 160, invert: true };
        await withLink(async (link) => {
            const handle = jogTo(link, invAxis, 0, 10, 10);
            const ok = await handle.done;
            expect(ok).toBe(true);

            await waitIdle(link, 5000);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            // Wire delta = targetCoordSteps * (invert ? -1 : 1) - wireCurrent
            //   = +1600 * (-1) - 0 = -1600 wire steps.
            // machinePos tracks wire steps directly.
            expect(st.pos![0]).toBeLessThan(-1000);
        }, { frameMs: 5 });
    });
});

describe("operatorJog: jogToPoint (coordinated multi-axis go-to)", () => {
    const cal = (stepsPerUnit: number, invert = false): AxisCalibration => ({ stepsPerUnit, invert });

    it("drives all four slots to their targets in one move", { timeout: 20000 }, async () => {
        await withLink(async (link) => {
            const handle = jogToPoint(link, [
                { axisIndex: 0, axis: cal(160), targetPos: 20 },
                { axisIndex: 1, axis: cal(160), targetPos: 20 },
                { axisIndex: 2, axis: cal(160), targetPos: 10 },
                { axisIndex: 3, axis: cal(10), targetPos: 80 },
            ], 40);
            expect(await handle.done).toBe(true);

            await waitIdle(link, 8000);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.pos!).toEqual([3200, 3200, 1600, 800]);
        }, { frameMs: 5 });
    });

    it("axes finish together — the minor axis is not left behind", { timeout: 20000 }, async () => {
        // The point of one coordinated move over per-axis moves: X travels 20 mm
        // and Y only 5 mm, so Y must be paced down to arrive with X rather than
        // finishing early and turning the diagonal into an L.
        await withLink(async (link) => {
            const handle = jogToPoint(link, [
                { axisIndex: 0, axis: cal(160), targetPos: 20 },
                { axisIndex: 1, axis: cal(160), targetPos: 5 },
            ], 40);
            expect(await handle.done).toBe(true);
            await waitIdle(link, 8000);
            const st = await link.getStatus();
            expect(st.pos![0]).toBe(3200);
            expect(st.pos![1]).toBe(800);
        }, { frameMs: 5 });
    });

    it("an unnamed axis is not commanded at all", { timeout: 15000 }, async () => {
        await withLink(async (link) => {
            const handle = jogToPoint(link, [{ axisIndex: 2, axis: cal(160), targetPos: 3 }], 20);
            expect(await handle.done).toBe(true);
            await waitIdle(link, 5000);
            const st = await link.getStatus();
            expect(st.pos![0]).toBe(0);
            expect(st.pos![1]).toBe(0);
            expect(st.pos![2]).toBe(480);
        }, { frameMs: 5 });
    });

    it("every axis already at target → no-op, resolves true", async () => {
        await withLink(async (link) => {
            const handle = jogToPoint(link, [
                { axisIndex: 0, axis: cal(160), targetPos: 0 },
                { axisIndex: 1, axis: cal(160), targetPos: 0 },
            ], 20);
            expect(await handle.done).toBe(true);
        });
    });

    it("invert applies per axis, not per move", { timeout: 15000 }, async () => {
        await withLink(async (link) => {
            const handle = jogToPoint(link, [
                { axisIndex: 0, axis: cal(160, true), targetPos: 10 },
                { axisIndex: 1, axis: cal(160, false), targetPos: 10 },
            ], 40);
            expect(await handle.done).toBe(true);
            await waitIdle(link, 6000);
            const st = await link.getStatus();
            expect(st.pos![0]).toBe(-1600);
            expect(st.pos![1]).toBe(1600);
        }, { frameMs: 5 });
    });
});