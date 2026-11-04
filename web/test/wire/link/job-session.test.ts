/**
 * Tests for a job session (a Session over a ReplayableSource), over the sim
 * through a real Link: it lives until cancel() or end(), holds on a pause and a tool
 * change, resumes at the status's resume point, and ends on a lost job.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../src/wire/link/link.js";
import { SimTransport } from "../../../src/wire/link/backends/sim.js";
import { JobSource, ListSource, FATAL_JOB_LOST, type Session } from "../../../src/wire/link/session.js";
import { MachineState, JobReason } from "../../../src/wire/format/status.js";
import { packBezier } from "../../../src/wire/format/bezier.js";
import { cubic } from "../../../src/toolpath/geometry.js";
import { packCjog } from "../../../src/wire/format/cjog.js";
import {
    CJOG_DEADMAN_MS, MAGIC_BEZIER, MAGIC_JOB_HDR, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR,
    NACK_DETAIL_AXIS_Y, NACK_DETAIL_SIDE_HI, NACK_JOB, NACK_OUT_OF_RANGE,
} from "../../../src/wire/format/constants.js";
import { packJobHdr, packLayerHdr, packToolHdr } from "../../../src/wire/format/layer.js";
import { stampSeq } from "../../../src/wire/format/packet.js";

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

const BOX = { minX: 0, minY: 0, maxX: 10, maxY: 10 };
const layerOf = (layer: number, tool: number, packets: Uint8Array[], bbox = BOX) =>
    ({ hdr: { layer }, tool, head: 0, bbox, packets });
const job = (n: number) => new JobSource([layerOf(0, 1, Array.from({ length: n }, (_, i) => rec(i + 1)))]);
/** JOB_HDR, its TOOL_HDR and the first layer's TOOL_HDR, ahead of LAYER_HDR and records. */
const HDRS = 3;

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

/** Sign off placement once the session holds for it; a session that ends first is left alone. */
async function signOff(link: Link, sess: Session, done: Promise<boolean>): Promise<void> {
    let ended = false;
    void done.then(() => { ended = true; });
    await until(() => ended || sess.held === "place");
    if (ended) return;
    expect(await link.command("place set")).toBe("ok");
    expect(await link.command("place ok")).toBe("ok");
}

async function start(link: Link, n: number): Promise<{ sess: Session; done: Promise<boolean> }> {
    await link.resetSeq();
    const sess = link.session(job(n), 4);
    const done = sess.run();
    await signOff(link, sess, done);
    return { sess, done };
}

const atX = async (link: Link, x: number) => (await link.getStatus()).wpos?.[0] === x;

describe("wire/link/session: job life", () => {
    it("stays open after the last ACK and ends on cancel()", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 6);
            await until(() => atX(link, 6));
            expect(sess.stats()).toMatchObject({ acked: HDRS + 7, held: null });
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
            await until(() => sess.stats().acked === HDRS + 2);
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
            await until(() => sess.stats().acked === HDRS + 2);
            await link.command("pause");
            await until(() => sess.held === "paused");
            sess.truncate();
            expect(await done).toBe(true);
        });
    });

    it("a job cancelled behind its back is FATAL_JOB_LOST", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => sess.stats().acked === HDRS + 2);
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
            await until(() => sess.stats().acked === HDRS + 11);
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
            await until(() => sess.stats().acked === HDRS + 11);
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

describe("wire/link/session: job headers", () => {
    const frames = (s: JobSource): Promise<Uint8Array[] | null> =>
        s.start().pull({ room: 100 } as never);

    it("emits JOB_HDR, one TOOL_HDR per tool with its union bbox, then each layer with its tool's header", async () => {
        const src = new JobSource([
            layerOf(0, 1, [rec(1)], { minX: 0, minY: 0, maxX: 5, maxY: 5 }),
            layerOf(1, 2, [rec(2)]),
            layerOf(2, 1, [rec(3)], { minX: -1, minY: 0, maxX: 7, maxY: 5 }),
        ]);
        const out = (await frames(src))!;
        expect(out.map((p) => p[0])).toEqual([
            MAGIC_JOB_HDR, MAGIC_TOOL_HDR, MAGIC_TOOL_HDR,
            MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER,
            MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER,
            MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER,
        ]);
        expect(src.preflightFrames).toBe(3);
        expect(out[0]![15]).toBe(2);
        // Each layer's TOOL_HDR names that layer's tool.
        expect([out[4]![1], out[7]![1], out[10]![1]]).toEqual([1, 2, 1]);
        const minX = new DataView(out[1]!.buffer).getFloat32(4, true);
        const maxX = new DataView(out[1]!.buffer).getFloat32(8, true);
        expect([minX, maxX]).toEqual([-1, 7]);
    });

    it("a resume is LAYER_HDR alone, then the later layers with their tool headers", async () => {
        const src = new JobSource([layerOf(0, 1, [rec(1), rec(2)]), layerOf(1, 2, [rec(3)])]);
        const out = (await src.from(0, 1).pull({ room: 100 } as never))!;
        expect(out.map((p) => p[0])).toEqual([MAGIC_LAYER_HDR, MAGIC_BEZIER, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER]);
        const atEnd = (await src.from(0, 2).pull({ room: 100 } as never))!;
        expect(atEnd.map((p) => p[0])).toEqual([MAGIC_LAYER_HDR, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER]);
        expect(await src.from(0, 3).pull({ room: 100 } as never)).toBeNull();
    });

    it("refuses a job of too many tools, or a tool on two heads", () => {
        expect(() => new JobSource([1, 2, 3, 4, 5].map((t) => layerOf(t, t, [])))).toThrow(RangeError);
        expect(() => new JobSource([layerOf(0, 1, []), { ...layerOf(1, 1, []), head: 1 }])).toThrow(RangeError);
    });

    it("a preflight refusal is fatal, with its detail", async () => {
        await withLink(async (link, sim) => {
            sim.refuseTool = { tool: 1, reason: NACK_OUT_OF_RANGE, detail: NACK_DETAIL_AXIS_Y | NACK_DETAIL_SIDE_HI };
            const { sess, done } = await start(link, 2);
            expect(await done).toBe(false);
            expect(sess.fatalReason).toBe(NACK_OUT_OF_RANGE);
            expect(sess.fatalDetail).toBe(NACK_DETAIL_AXIS_Y | NACK_DETAIL_SIDE_HI);
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("NACK_JOB in preflight is fatal, not a hold", async () => {
        await withLink(async (link, sim) => {
            sim.refuseTool = { tool: 1, reason: NACK_JOB };
            const { sess, done } = await start(link, 2);
            expect(await done).toBe(false);
            expect(sess.fatalReason).toBe(NACK_JOB);
        });
    });

    it("refuses a pause before the first layer opens", async () => {
        await withLink(async (link, sim) => {
            await link.resetSeq();
            await sim.write(stampSeq(packJobHdr({ recordCount: 0, toolCount: 1 }), 0));
            expect(await link.command("pause")).toMatch(/^err/);
            expect((await link.getStatus()).reason).toBe(JobReason.PREFLIGHT);
            await sim.write(stampSeq(packToolHdr({ tool: 1, head: 0, bbox: BOX }), 1));
            expect((await link.getStatus()).reason).toBe(JobReason.PLACE);
            expect(await link.command("pause")).toMatch(/^err/);
            expect(await link.command("cancel")).toBe("ok");
        });
    });

    it("a pause with the next layer already sent resumes, not lost", async () => {
        await withLink(async (link) => {
            await link.resetSeq();
            const sess = link.session(new JobSource([
                layerOf(0, 1, Array.from({ length: 20 }, (_, i) => rec(i + 1))),
                layerOf(1, 2, Array.from({ length: 20 }, (_, i) => rec(i + 21))),
            ]), 8);
            const done = sess.run();
            await signOff(link, sess, done);
            await until(async () => (await link.getStatus()).reason === JobReason.RUNNING);
            expect(await link.command("pause")).toBe("ok");
            await until(() => sess.held === "paused");
            expect(sess.resume()).toBe(true);
            await until(() => atX(link, 40));
            expect(sess.fatalReason).toBeUndefined();
            await sess.cancel();
            expect(await done).toBe(true);
        }, { recordMs: 20, ringSize: 30 });
    });
});

describe("wire/link/session: manual layers", () => {
    const twoLayers = () => new JobSource([
        layerOf(0, 1, [rec(1), rec(2)]), layerOf(1, 1, [rec(3), rec(4)]),
    ], 0, { manual: true });

    it("sends each layer only on release()", async () => {
        await withLink(async (link) => {
            await link.resetSeq();
            const src = twoLayers();
            const sess = link.session(src, 4);
            const done = sess.run();
            await signOff(link, sess, done);
            await tick(60);
            expect(sess.stats().emitted).toBe(src.preflightFrames);
            expect(src.waiting).toBe(true);

            src.release();
            await until(() => atX(link, 2));
            await tick(60);
            expect((await link.getStatus()).wpos?.[0]).toBe(2);
            expect(src.waiting).toBe(true);
            expect(src.drained).toBe(false);

            src.release();
            await until(() => atX(link, 4));
            expect(src.drained).toBe(true);
            expect(src.waiting).toBe(false);
            expect(await sess.cancel()).toBe("ok");
            expect(await done).toBe(true);
        });
    });

    it("clearing manual lets the remaining layers run", async () => {
        await withLink(async (link) => {
            await link.resetSeq();
            const src = twoLayers();
            const sess = link.session(src, 4);
            const done = sess.run();
            await signOff(link, sess, done);
            src.release();
            await until(() => atX(link, 2));
            src.manual = false;
            await until(() => atX(link, 4));
            expect(await sess.cancel()).toBe("ok");
            await done;
        });
    });

    it("a resume at a layer boundary keeps the later layers back", async () => {
        const src = twoLayers();
        const ctx = { room: 100 } as never;
        const first = src.start();
        await first.pull(ctx);
        src.release();
        await first.pull(ctx);
        const out = (await src.from(0, 2).pull(ctx))!;
        expect(out.map((p) => p[0])).toEqual([MAGIC_LAYER_HDR]);
        expect(src.waiting).toBe(true);
        src.release();
        expect(src.waiting).toBe(false);
    });

    it("is off by default", async () => {
        const src = new JobSource([layerOf(0, 1, [rec(1)]), layerOf(1, 1, [rec(2)])]);
        const stream = src.start();
        const out = (await stream.pull({ room: 100 } as never))!;
        expect(out.length).toBe(src.preflightFrames + 6);
        expect(src.drained).toBe(true);
        expect(src.waiting).toBe(false);
    });
});

describe("wire/link/session: placement", () => {
    const open = async (link: Link) => {
        await link.resetSeq();
        const sess = link.session(job(3), 4);
        const done = sess.run();
        await until(() => sess.held === "place");
        return { sess, done };
    };

    it("sends no layer until `place ok`, then runs the job", async () => {
        await withLink(async (link) => {
            const { sess, done } = await open(link);
            await tick(150);
            expect(sess.stats()).toMatchObject({ sent: 2, acked: 2, nacks: 0, held: "place" });
            expect(sess.resume()).toBe(false);
            expect(await link.command("place ok")).toBe("err bad_state"); // no zero set
            expect(await link.command("place set")).toBe("ok");
            expect(await link.command("place go xmax ymax")).toBe("ok");
            expect(await link.command("place frame")).toBe("ok");
            await tick(150);
            expect(sess.held).toBe("place");
            expect(await link.command("place ok")).toBe("ok");
            await until(() => atX(link, 3));
            expect(sess.held).toBeNull();
            await sess.cancel();
            expect(await done).toBe(true);
        });
    });

    it("a jog holds on, and `place reset` lets it jog again", async () => {
        await withLink(async (link) => {
            await link.command("setorigin");
            const { sess, done } = await open(link);
            expect(await link.command("jog z 1")).toBe("err bad_state");
            expect(await link.command("jog x 5 100")).toMatch(/^ok/);
            expect(await link.command("place set")).toBe("err busy");
            await until(async () => (await link.getStatus()).state === MachineState.JOB);
            expect(sess.held).toBe("place");
            expect(await link.command("place set")).toBe("ok");
            expect(await link.command("jog x 1")).toBe("err bad_state");
            expect(await link.command("place reset")).toBe("ok");
            expect(await link.command("jog x 1 100")).toMatch(/^ok/);
            await until(async () => (await link.getStatus()).state === MachineState.JOB);
            await sess.cancel();
            expect(await done).toBe(true);
        }, { frameMs: 5, cjogStepsPerS: 1000 });
    });

    it("cancel() is refused while moving and the hold goes on; end() stops it", async () => {
        await withLink(async (link) => {
            await link.command("setorigin");
            const { sess, done } = await open(link);
            expect(await link.command("jog x 5000 1")).toMatch(/^ok/);
            expect(await sess.cancel()).toBe("err bad_state");
            await tick(150);
            expect(sess.held).toBe("place");
            sess.end();
            expect(await done).toBe(true);
            expect(sess.fatalReason).toBeUndefined();
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        }, { frameMs: 5, cjogStepsPerS: 1000 });
    });

    it("refuses layers and bad usage", async () => {
        await withLink(async (link, sim) => {
            await link.resetSeq();
            await sim.write(stampSeq(packJobHdr({ recordCount: 0, toolCount: 1 }), 0));
            await sim.write(stampSeq(packToolHdr({ tool: 1, head: 0, bbox: BOX }), 1));
            expect(await link.command("place go xmax")).toBe("err usage");
            await link.resetSeq();
            const layer = link.session(new ListSource([packLayerHdr({ layer: 0 })]));
            expect(await layer.run()).toBe(false);
            expect(layer.fatalReason).toBe(NACK_JOB);
            const st = await link.getStatus();
            expect([st.state, st.reason]).toEqual([MachineState.JOB, JobReason.PLACE]);
            expect(await link.command("cancel")).toBe("ok");
        });
    });

    it("cancel() and end() leave the hold; a cancel behind its back is lost", async () => {
        await withLink(async (link) => {
            const a = await open(link);
            expect(await a.sess.cancel()).toBe("ok");
            expect(await a.done).toBe(true);

            const b = await open(link);
            b.sess.end();
            expect(await b.done).toBe(true);
            expect(b.sess.fatalReason).toBeUndefined();

            const c = await open(link);
            expect(await link.command("cancel")).toBe("ok");
            expect(await c.done).toBe(false);
            expect(c.sess.fatalReason).toBe(FATAL_JOB_LOST);
        });
    });

    it("locks XY in TOOL_WAIT, Z alone may be zeroed", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 3);
            await until(async () => (await link.getStatus()).reason === JobReason.TOOL_WAIT);
            expect(await link.command("wzero x")).toBe("err bad_state");
            expect(await link.command("wzero")).toBe("err bad_state");
            expect(await link.command("wzero z")).toBe("ok");
            await sess.cancel();
            expect(await done).toBe(true);
        }, { tools: {}, toolChangeMs: 20 });
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

    it("zeroes another head after selecting it in TOOL_WAIT", async () => {
        await withLink(async (link, sim) => {
            const { sess, done } = await start(link, 3);
            await until(async () => (await link.getStatus()).reason === JobReason.TOOL_WAIT);
            expect(await link.command("select 1")).toBe("ok");
            expect(await link.command("wzero z")).toBe("ok");
            expect(sim.zeroed.has(1)).toBe(true);
            expect(sim.zeroed.has(0)).toBe(false);
            await sess.cancel();
            expect(await done).toBe(true);
        }, { tools: {}, toolChangeMs: 20 });
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
