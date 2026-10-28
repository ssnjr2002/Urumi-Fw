/**
 * Tests for a job session (a Session over a ReplayableSource), over the sim
 * through a real Link: it lives until cancel() or end(), holds on a pause and a tool
 * change, resumes at the status's resume point, and ends on a lost job.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../src/wire/link/link.js";
import { SimTransport } from "../../../src/wire/link/backends/sim.js";
import { JobSource, FATAL_JOB_LOST, type Session } from "../../../src/wire/link/session.js";
import { MachineState, JobReason } from "../../../src/wire/format/status.js";
import { packBezier } from "../../../src/wire/format/bezier.js";
import { cubic } from "../../../src/toolpath/geometry.js";
import { packCjog } from "../../../src/wire/format/cjog.js";
import { CJOG_DEADMAN_MS } from "../../../src/wire/format/constants.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

/** A straight BEZIER record along X ending at `x` mm (= steps at jogSpm 1). */
function rec(x: number): Uint8Array {
    return packBezier({
        curve: cubic({ x: x - 1, y: 0 }, { x: x - 2 / 3, y: 0 }, { x: x - 1 / 3, y: 0 }, { x, y: 0 }),
        flags: 0, nextBreakDist: 0, sweepMin: 0, sweepMax: 0,
        length: 1, kappaMax: 0, dkappaMax: 0, ts: [1, 0, 0],
        kappaStart: 0, kappaEnd: 0, fitError: 0,
    });
}

const job = (n: number) => new JobSource({ tool: 1, head: 0 }, Array.from({ length: n }, (_, i) => rec(i + 1)));

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    simOpts?: ConstructorParameters<typeof SimTransport>[0],
): Promise<T> {
    const sim = new SimTransport({ axisMap: [1, 2, 3, 4], recordMs: 5, bufferIdleMs: 20, ...simOpts });
    const link = new Link(sim);
    try {
        return await fn(link, sim);
    } finally {
        await link.close();
    }
}

/** Wait until `cond` holds, or fail after `ms`. */
async function until(cond: () => boolean | Promise<boolean>, ms = 3000): Promise<void> {
    const deadline = Date.now() + ms;
    while (!(await cond())) {
        if (Date.now() > deadline) throw new Error("timed out");
        await tick(5);
    }
}

async function start(link: Link, n: number): Promise<{ sess: Session; done: Promise<boolean> }> {
    await link.resetSeq();
    const sess = link.session(job(n), 4);
    return { sess, done: sess.run() };
}

const atX = async (link: Link, x: number) => (await link.getStatus()).wpos?.[0] === x;

describe("wire/link/session: job life", () => {
    it("stays open after the last ACK and ends on cancel()", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 6);
            await until(() => atX(link, 6));
            expect(sess.stats()).toMatchObject({ acked: 7, held: null });
            let ended = false;
            void done.then(() => { ended = true; });
            await tick(100);
            expect(ended).toBe(false);
            expect(await sess.cancel()).toBe("ok");
            expect(await done).toBe(true);
            expect(sess.fatalReason).toBeUndefined();
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("end() ends a running job", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 40);
            await until(async () => (await link.getStatus()).reason === JobReason.RUNNING);
            sess.end();
            expect(await done).toBe(true);
            expect(sess.fatalReason).toBeUndefined();
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("end() ends a held session", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => sess.stats().acked === 2);
            await link.command("pause");
            await until(() => sess.held === "paused");
            sess.end();
            expect(await done).toBe(true);
            expect(sess.fatalReason).toBeUndefined();
        });
    });

    it("truncate() ends a held session", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => sess.stats().acked === 2);
            await link.command("pause");
            await until(() => sess.held === "paused");
            sess.truncate();
            expect(await done).toBe(true);
        });
    });

    it("a job cancelled behind its back is FATAL_JOB_LOST", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => sess.stats().acked === 2);
            await link.command("pause");
            await until(() => sess.held === "paused");
            expect(await link.command("cancel")).toBe("ok");
            expect(await done).toBe(false);
            expect(sess.fatalReason).toBe(FATAL_JOB_LOST);
        });
    });
});

describe("wire/link/session: pause", () => {
    it("holds on NACK_PAUSED mid-stream and resumes at the resume point", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 40);
            await until(async () => (await link.getStatus()).reason === JobReason.RUNNING);
            expect(await link.command("pause")).toBe("ok");
            await until(() => sess.held === "paused");
            expect(sess.stats().nacks).toBeGreaterThan(0); // held by NACK_PAUSED, not the status
            expect(sess.resume()).toBe(true);
            await until(() => atX(link, 40));
            expect(sess.held).toBeNull();
            await sess.cancel();
            expect(await done).toBe(true);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("holds on a status PAUSED after the last ACK", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 10);
            await until(() => sess.stats().acked === 11);
            expect(await link.command("pause")).toBe("ok");
            await until(() => sess.held === "paused");
            sess.resume();
            await until(() => atX(link, 10));
            await sess.cancel();
            expect(await done).toBe(true);
        }, { recordMs: 30 });
    });
});

describe("wire/link/session: pause()", () => {
    it("holds on its own ok after the last ACK and resumes", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 10);
            await until(() => sess.stats().acked === 11);
            expect(await sess.pause()).toBe("ok");
            await until(() => sess.held === "paused");
            sess.resume();
            await until(() => atX(link, 10));
            expect(sess.held).toBeNull();
            await sess.cancel();
            expect(await done).toBe(true);
        }, { recordMs: 30 });
    });

    it("mid-stream, resumes once and does not hold again", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 40);
            await until(async () => (await link.getStatus()).reason === JobReason.RUNNING);
            expect(await sess.pause()).toBe("ok");
            await until(() => sess.held === "paused");
            sess.resume();
            await until(() => atX(link, 40));
            expect(sess.held).toBeNull();
            await sess.cancel();
            expect(await done).toBe(true);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("an err reply does not hold", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => atX(link, 1));
            await until(async () => (await link.getStatus()).reason === JobReason.BUFFERING);
            await link.command("pause");
            await until(() => sess.held === "paused");
            expect(await sess.pause()).toMatch(/^err/); // already paused
            sess.resume();
            await until(() => sess.held === null);
            await tick(150);
            expect(sess.held).toBeNull();
            sess.truncate();
            expect(await done).toBe(true);
        });
    });
});

describe("wire/link/session: tool change", () => {
    it("waits in TOOL_WAIT for resume(), and again until the tool is mounted and zeroed", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 3);
            await until(() => sess.held === "tool");
            await until(async () => (await link.getStatus()).reason === JobReason.TOOL_WAIT);
            // Nothing is resent without the operator.
            const sent = sess.stats().sent;
            await tick(150);
            expect(sess.stats().sent).toBe(sent);

            expect(sess.resume()).toBe(true); // not mounted yet: refused again
            await until(() => sess.stats().sent > sent);
            await until(() => sess.held === "tool");

            expect(await link.command("mount 0 1")).toBe("ok");
            expect(await link.command("wzero z")).toBe("ok");
            sess.resume();
            await until(() => atX(link, 3));
            expect(sess.held).toBeNull();
            await sess.cancel();
            expect(await done).toBe(true);
        }, { tools: {} });
    });

    it("refuses resume() during TOOL_CHANGE, and end() leaves the hold", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 3);
            await until(() => sess.held === "tool");
            await until(async () => (await link.getStatus()).reason === JobReason.TOOL_CHANGE);
            await until(() => sess.stats().held === "tool");
            expect(sess.resume()).toBe(false);
            sess.end();
            expect(await done).toBe(true);
            expect(sess.fatalReason).toBeUndefined();
        }, { tools: {}, toolChangeMs: 2000 });
    });

    it("a jog in TOOL_WAIT keeps the hold, and resume() after it resumes", async () => {
        await withLink(async (link, sim) => {
            await link.command("setorigin");
            const { sess, done } = await start(link, 3);
            await until(async () => (await link.getStatus()).reason === JobReason.TOOL_WAIT);
            const sent = sess.stats().sent;
            await sim.write(packCjog(1, 0));
            await until(async () => (await link.getStatus()).state === MachineState.JOGGING);
            expect(sess.resume()).toBe(false);
            for (let t = 0; t < 400; t += 30) { // held past several of the session's polls
                await sim.write(packCjog(1, 0));
                await tick(30);
            }
            await until(async () => (await link.getStatus()).state === MachineState.JOB, CJOG_DEADMAN_MS + 500);
            expect(sess.held).toBe("tool");
            expect(sess.stats().sent).toBe(sent); // nothing resent during the jog

            expect(await link.command("mount 0 1")).toBe("ok");
            expect(await link.command("wzero z")).toBe("ok");
            expect(sess.resume()).toBe(true);
            await until(() => sess.held === null);
            await sess.cancel();
            expect(await done).toBe(true);
            expect(sess.fatalReason).toBeUndefined();
        }, { tools: {}, toolChangeMs: 20, frameMs: 5, cjogStepsPerS: 1000, cjogTravel: [[0, 400], [0, 400]] });
    });

    it("a mounted, zeroed tool is ACKed with no hold", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 2);
            await until(() => atX(link, 2));
            expect(sess.stats().nacks).toBe(0);
            await sess.cancel();
            expect(await done).toBe(true);
        }, { tools: { mounted: { 0: 1 }, zeroed: [0] } });
    });
});
