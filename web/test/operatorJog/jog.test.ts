/**
 * Tests for operatorJog — the go-to helpers over `jog`, and the sim's `jog`,
 * over the in-process Sim.
 */

import { describe, it, expect, vi } from "vitest";
import { Link } from "../../src/wire/link/link.js";
import { SimTransport } from "../../src/wire/link/backends/sim.js";
import { jogTo, jogToPoint, type JogTarget } from "../../src/operatorJog/jogTo.js";
import { jog } from "../../src/wire/link/commands.js";
import { MachineState } from "../../src/wire/format/status.js";
import type { AxisCalibration } from "../../src/operatorJog/types.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

const cal = (stepsPerUnit: number, invert = false, invertDir = false): AxisCalibration =>
    ({ stepsPerUnit, invert, invertDir });
/** The Pico's signed steps/unit for `cal`. */
const spm = (c: AxisCalibration) => c.stepsPerUnit * (c.invertDir ? -1 : 1);

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    jogSpm?: readonly number[],
): Promise<T> {
    // Configured boot: jogs need bound slots.
    const sim = new SimTransport({
        axisMap: [1, 2, 3, 4], jogUnhomed: true, frameMs: 5, cjogStepsPerS: 20000, jogSpm,
    });
    const link = new Link(sim);
    try {
        return await fn(link, sim);
    } finally {
        await link.close();
    }
}

describe("operatorJog: jogToPoint", () => {
    const x = cal(160), y = cal(160), z = cal(1200), a = cal(10);
    const cases: Array<{ name: string; targets: JogTarget[]; pos: number[] }> = [
        { name: "one axis", targets: [{ axisIndex: 0, axis: x, targetPos: 10 }], pos: [1600, 0, 0, 0] },
        { name: "all four slots",
          targets: [
              { axisIndex: 0, axis: x, targetPos: 20 },
              { axisIndex: 1, axis: y, targetPos: 5 },
              { axisIndex: 2, axis: z, targetPos: -2 },
              { axisIndex: 3, axis: a, targetPos: 80 },
          ],
          pos: [3200, 800, -2400, 800] },
        { name: "an unnamed axis holds", targets: [{ axisIndex: 2, axis: z, targetPos: 1 }], pos: [0, 0, 1200, 0] },
        { name: "invert flips the wire frame",
          targets: [{ axisIndex: 0, axis: cal(160, true), targetPos: 10 }], pos: [-1600, 0, 0, 0] },
        { name: "invertDir flips the jog distance, not the target",
          targets: [{ axisIndex: 2, axis: cal(1200, false, true), targetPos: 1 }], pos: [0, 0, 1200, 0] },
    ];
    for (const c of cases) {
        it(c.name, { timeout: 10000 }, async () => {
            const jogSpm = [0, 1, 2, 3].map(k => {
                const t = c.targets.find(t => t.axisIndex === k);
                return t ? spm(t.axis) : 1;
            });
            await withLink(async (link) => {
                expect(await jogToPoint(link, c.targets, 20).done).toBe(true);
                const st = await link.getStatus();
                expect(st.state).toBe(MachineState.IDLE);
                expect(st.pos).toEqual(c.pos);
            }, jogSpm);
        });
    }

    it("every axis at its target sends nothing and resolves true", async () => {
        await withLink(async (link) => {
            const command = vi.spyOn(link, "command");
            expect(await jogTo(link, x, 0, 0, 10).done).toBe(true);
            expect(command).not.toHaveBeenCalled();
        });
    });

    it("abort stops the move, resolves false, and lands IDLE", { timeout: 5000 }, async () => {
        await withLink(async (link) => {
            const h = jogTo(link, x, 0, 1000, 20);
            await tick(50);
            h.abort();
            expect(await h.done).toBe(false);
            await tick(50);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.pos![0]).toBeLessThan(160000);
        }, [160, 160, 1, 1]);
    });
});

describe("sim: jog", () => {
    const refusals: Array<{ cmd: string; reply: string }> = [
        { cmd: "jog", reply: "err usage" },
        { cmd: "jog q 1", reply: "err usage" },
        { cmd: "jog xy 1", reply: "err usage" },
        { cmd: "jog x 0", reply: "err usage" },
        { cmd: "jog x 1 0", reply: "err usage" },
    ];
    for (const r of refusals) {
        it(`${r.cmd} → ${r.reply}`, async () => {
            await withLink(async (link) => {
                expect(await link.command(r.cmd)).toBe(r.reply);
            });
        });
    }

    it("joins up to four blocks, then busy", async () => {
        await withLink(async (link) => {
            const depths: number[] = [];
            for (let i = 0; i < 4; i++) depths.push(await jog(link, "a", 1000));
            expect(depths).toEqual([1, 2, 3, 4]);
            await expect(jog(link, "z", 1)).rejects.toThrow(/err busy/);
            expect(await link.command("get state jogging")).toBe("state=7 jogging=1");
        });
    });

    it("refuses an unhomed axis without jogUnhomed", async () => {
        const sim = new SimTransport({ axisMap: [1, 2, 3, 4] });
        const link = new Link(sim);
        expect(await link.command("jog z 1")).toBe("err not_homed");
        await link.close();
    });
});
