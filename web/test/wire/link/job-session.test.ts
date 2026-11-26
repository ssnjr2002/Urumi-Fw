/**
 * Tests for a job session (a Session over a ReplayableSource), over the sim
 * through a real Link: it lives until close() or end(), holds on a pause and a tool
 * change, resumes at the status's resume point, and ends on a lost job.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../src/link/link.js";
import { SimTransport } from "../../../src/transport/sim.js";
import { JobSource, jobIdOf, type Session, type SessionResult } from "../../../src/link/session.js";
import { Nack, Refusal, packNack } from "../../../src/packets/nack.js";
import { MachineState, JobReason } from "../../../src/packets/status.js";
import { packBezier } from "../../../src/packets/bezier.js";
import { cubic } from "../../../src/toolpath/geometry.js";
import { packCjog } from "../../../src/packets/cjog.js";
import {
    CJOG_DEADMAN_MS, MAGIC_BEZIER, MAGIC_JOB_HDR, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR,
} from "../../../src/packets/constants.js";
import { packJobHdr, packLayerHdr, packToolHdr } from "../../../src/packets/layer.js";
import { stampSeq } from "../../../src/packets/packet.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

/** A straight BEZIER record along X ending at `x` mm (= steps at jogSpm 1). */
function rec(x: number): Uint8Array {
    return packBezier({
        curve: cubic({ x: x - 1, y: 0 }, { x: x - 2 / 3, y: 0 }, { x: x - 1 / 3, y: 0 }, { x, y: 0 }),
        flags: 0, nextBreakDist: 0, sweepMin: 0, sweepMax: 0,
        length: 1, kappaMax: 0, dkappaMax: 0, ts: [1, 0, 0],
    });
}

const BOX = { minX: 0, minY: 0, maxX: 10, maxY: 10 };
const layerOf = (layer: number, tool: number, packets: Uint8Array[], bbox = BOX) =>
    ({ hdr: { layer }, tool, head: 0, bbox, records: packets });
const job = (n: number) => new JobSource([layerOf(0, 1, Array.from({ length: n }, (_, i) => rec(i + 1)))]);
/** JOB_HDR, its TOOL_HDR and the first layer's TOOL_HDR, ahead of LAYER_HDR and records. */
const HDRS = 3;

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    simOpts?: ConstructorParameters<typeof SimTransport>[0],
): Promise<T> {
    const sim = new SimTransport({ axesMap: [1, 2, 3, 4], recordMs: 5, bufferIdleMs: 20, ...simOpts });
    const link = new Link(sim);
    try {
        await link.commands().send("setorigin");   // `wpos` tracks the job only on homed axes
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
async function signOff(link: Link, sess: Session, done: Promise<SessionResult>): Promise<void> {
    let ended = false;
    void done.then(() => { ended = true; }, () => { ended = true; });
    await until(() => ended || sess.hold === "place");
    if (ended) return;
    expect((await link.freshStatus()).placed).toBe(false);
    expect(await link.commands().send("place set")).toBe("ok");
    expect((await link.freshStatus()).placed).toBe(true);
    expect(await link.commands().send("place ok")).toBe("ok");
    expect((await link.freshStatus()).placed).toBe(false);
}

async function start(link: Link, n: number): Promise<{ sess: Session; done: Promise<SessionResult> }> {
    const sess = link.session(job(n));
    const done = sess.run();
    await signOff(link, sess, done);
    return { sess, done };
}

const atX = async (link: Link, x: number) => (await link.freshStatus()).wpos?.[0] === x;

describe("wire/link/session: job life", () => {
    it("stays open after the last ACK and ends on close()", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 6);
            expect(sess.done).toBe(false);
            await until(() => atX(link, 6));
            expect(sess.stats()).toMatchObject({ acked: HDRS + 7 });
            expect(sess.drained).toBe(true);
            await until(() => sess.done);
            expect(sess.hold).toBeNull();
            let ended = false;
            void done.then(() => { ended = true; }, () => { ended = true; });
            await tick(100);
            expect(ended).toBe(false);
            expect(await sess.close()).toBe("ok");
            expect((await done).ok).toBe(true);
            expect((await link.freshStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("abort() ends a running job", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 40);
            await until(async () => (await link.freshStatus()).reason === JobReason.RUNNING);
            sess.abort();
            expect((await done).ok).toBe(true);
            expect((await link.freshStatus()).state).toBe(MachineState.IDLE);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("abort() ends a held session", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => sess.stats().acked === HDRS + 2);
            await link.commands().send("pause");
            await until(() => sess.hold === "paused");
            sess.abort();
            expect((await done).ok).toBe(true);
        });
    });

    it("rejects when the link closes, leaving the job open", async () => {
        const sim = new SimTransport({ axesMap: [1, 2, 3, 4], recordMs: 5, bufferIdleMs: 20 });
        const link = new Link(sim);
        const { sess, done } = await start(link, 1);
        await until(() => sess.drained);
        await link.close();
        await expect(done).rejects.toThrow("the link closed");
        expect(sim.state).toBe(MachineState.JOB);
    });

    it("a job cancelled behind its back is job-lost", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => sess.stats().acked === HDRS + 2);
            await link.commands().send("pause");
            await until(() => sess.hold === "paused");
            expect(await link.commands().send("cancel")).toBe("ok");
            const r = await done;
            expect(r.fatal).toEqual({ by: "host", why: "job-lost" });
            expect(r.lastLayer).toBe(0);
            expect(r.lastRecord).toBeDefined();
        });
    });
});

describe("wire/link/session: attach", () => {
    /** A job paused mid-stream, its first session gone. */
    async function pausedAway(link: Link, n: number): Promise<void> {
        const { sess, done } = await start(link, n);
        await until(async () => (await link.freshStatus()).reason === JobReason.RUNNING);
        expect(await link.commands().send("pause")).toBe("ok");
        await until(() => sess.hold === "paused");
        sess._drop();
        await done;
    }

    it("takes over a paused job with the same id and resumes it", async () => {
        await withLink(async (link) => {
            await pausedAway(link, 40);
            expect(await link.commands().send("get jobid")).toBe(`jobid=${job(40).jobId.toString(16).padStart(8, "0")}`);
            const sess = link.session(job(40));
            const done = sess.run();
            await until(() => sess.hold === "paused");
            expect(await sess.resume()).toBe(true);
            await until(() => atX(link, 40));
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("waits on a job still cutting until it pauses", async () => {
        await withLink(async (link) => {
            const first = await start(link, 40);
            await until(async () => (await link.freshStatus()).reason === JobReason.RUNNING);
            first.sess._drop();
            await first.done;
            const sess = link.session(job(40));
            const done = sess.run();
            await tick(100);
            expect(sess.hold).toBeNull();
            expect(sess.stats().sent).toBe(0);
            // The sim has no host watch: pause it by hand, as a dry job would.
            expect(await link.commands().send("pause")).toBe("ok");
            await until(() => sess.hold === "paused");
            expect(await sess.resume()).toBe(true);
            await until(() => atX(link, 40));
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("refuses a job with another id, sending nothing", async () => {
        await withLink(async (link) => {
            await pausedAway(link, 40);
            const sess = link.session(job(41));
            expect((await sess.run()).fatal).toEqual({ by: "host", why: "job-mismatch" });
            expect(sess.stats().sent).toBe(0);
            expect((await link.freshStatus()).reason).toBe(JobReason.PAUSED);
        }, { recordMs: 20, ringSize: 8 });
    });
});

describe("wire/link/session: job id", () => {
    it("ignores seq and CRC8, and changes with a record", () => {
        const a = job(3);
        expect(a.jobId).toBe(job(3).jobId);
        expect(a.jobId).not.toBe(job(4).jobId);
        const r = rec(1);
        expect(jobIdOf([stampSeq(r, 0)])).toBe(jobIdOf([stampSeq(r, 9)]));
    });
});

describe("wire/link/session: pause", () => {
    it("holds on NACK_PAUSED mid-stream and resumes at the resume point", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 40);
            await until(async () => (await link.freshStatus()).reason === JobReason.RUNNING);
            expect(await link.commands().send("pause")).toBe("ok");
            await until(() => sess.hold === "paused");
            expect(sess.stats().nacks).toBeGreaterThan(0); // held by NACK_PAUSED, not the status
            expect(await sess.resume()).toBe(true);
            await until(() => atX(link, 40));
            expect(sess.hold).toBeNull();
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("holds on a status PAUSED after the last ACK", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 10);
            await until(() => sess.stats().acked === HDRS + 11);
            expect(await link.commands().send("pause")).toBe("ok");
            await until(() => sess.hold === "paused");
            sess.resume();
            await until(() => atX(link, 10));
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 30 });
    });
});

describe("wire/link/session: pause()", () => {
    it("holds on its own ok after the last ACK and resumes", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 10);
            await until(() => sess.stats().acked === HDRS + 11);
            expect(await sess.pause()).toBe("ok");
            await until(() => sess.hold === "paused");
            sess.resume();
            await until(() => atX(link, 10));
            expect(sess.hold).toBeNull();
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 30 });
    });

    it("mid-stream, resumes once and does not hold again", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 40);
            await until(async () => (await link.freshStatus()).reason === JobReason.RUNNING);
            expect(await sess.pause()).toBe("ok");
            await until(() => sess.hold === "paused");
            sess.resume();
            await until(() => atX(link, 40));
            expect(sess.hold).toBeNull();
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 20, ringSize: 8 });
    });

    it("an err reply does not hold", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 1);
            await until(() => atX(link, 1));
            await until(async () => (await link.freshStatus()).reason === JobReason.BUFFERING);
            await link.commands().send("pause");
            await until(() => sess.hold === "paused");
            expect(await sess.pause()).toMatch(/^err/); // already paused
            sess.resume();
            await until(() => sess.hold === null);
            await tick(150);
            expect(sess.hold).toBeNull();
            sess.abort();
            expect((await done).ok).toBe(true);
        });
    });
});

describe("wire/link/session: job headers", () => {
    const frames = (s: JobSource): Uint8Array[] | null => s.start().pull(100);

    it("emits JOB_HDR, one TOOL_HDR per tool with its union bbox, then each layer with its tool's header", async () => {
        const src = new JobSource([
            layerOf(0, 1, [rec(1)], { minX: 0, minY: 0, maxX: 5, maxY: 5 }),
            layerOf(1, 2, [rec(2)]),
            layerOf(2, 1, [rec(3)], { minX: -1, minY: 0, maxX: 7, maxY: 5 }),
        ]);
        const out = frames(src)!;
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
        const out = src.from(0, 1).pull(100)!;
        expect(out.map((p) => p[0])).toEqual([MAGIC_LAYER_HDR, MAGIC_BEZIER, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER]);
        const atEnd = src.from(0, 2).pull(100)!;
        expect(atEnd.map((p) => p[0])).toEqual([MAGIC_LAYER_HDR, MAGIC_LAYER_HDR, MAGIC_TOOL_HDR, MAGIC_BEZIER]);
        expect(src.from(0, 3).pull(100)).toBeNull();
    });

    it("refuses a job of too many tools, or a tool on two heads", () => {
        expect(() => new JobSource([1, 2, 3, 4, 5].map((t) => layerOf(t, t, [])))).toThrow(RangeError);
        expect(() => new JobSource([layerOf(0, 1, []), { ...layerOf(1, 1, []), head: 1 }])).toThrow(RangeError);
    });

    it("a preflight refusal is fatal, with its text", async () => {
        await withLink(async (link, sim) => {
            sim.refuseTool = { tool: 1, code: Nack.OUT_OF_RANGE, text: "y high, tool 1" };
            const { done } = await start(link, 2);
            expect((await done).fatal).toEqual({
                by: "controller", nack: Nack.OUT_OF_RANGE, text: "y high, tool 1",
            });
            expect((await link.freshStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("a refusal after preflight other than NO_RESUME ends the session, not a hold", async () => {
        await withLink(async (link, sim) => {
            // The first batch after placement is refused ORDER, as the Pico
            // would a record out of order. Batches are whole frames.
            const s = sim as unknown as { write: (d: Uint8Array) => Promise<void>; reply: (d: Uint8Array) => void };
            const write = s.write.bind(sim);
            let refused = false;
            s.write = async (d) => {
                if (d[0] === MAGIC_LAYER_HDR && !refused) {
                    refused = true;
                    s.reply(packNack(Nack.ORDER, "no layer open"));
                    return;
                }
                await write(d);
            };
            const { sess, done } = await start(link, 4);
            expect((await done).fatal).toEqual({ by: "controller", nack: Nack.ORDER, text: "no layer open" });
            expect(sess.hold).toBeNull();
        });
    });

    it("NO_RESUME holds the session `refused`", async () => {
        await withLink(async (link, sim) => {
            const s = sim as unknown as { write: (d: Uint8Array) => Promise<void>; reply: (d: Uint8Array) => void };
            const write = s.write.bind(sim);
            let refused = false;
            s.write = async (d) => {
                if (d[0] === MAGIC_LAYER_HDR && !refused) {
                    refused = true;
                    s.reply(packNack(Nack.NO_RESUME));
                    return;
                }
                await write(d);
            };
            const { sess, done } = await start(link, 4);
            await until(() => sess.hold === "refused");
            sess.abort();
            await done;
        });
    });

    it("TOOL_LIST in preflight is fatal, not a hold", async () => {
        await withLink(async (link, sim) => {
            sim.refuseTool = { tool: 1, code: Nack.TOOL_LIST };
            const { done } = await start(link, 2);
            expect((await done).fatal).toMatchObject({ by: "controller", nack: Nack.TOOL_LIST });
        });
    });

    it("refuses a pause before the first layer opens", async () => {
        await withLink(async (link, sim) => {
            await link.resetSeq();
            await sim.write(stampSeq(packJobHdr({ recordCount: 0, toolCount: 1 }), 0));
            expect(await link.commands().send("pause")).toMatch(/^err/);
            expect((await link.freshStatus()).reason).toBe(JobReason.PREFLIGHT);
            await sim.write(stampSeq(packToolHdr({ tool: 1, head: 0, bbox: BOX }), 1));
            expect((await link.freshStatus()).reason).toBe(JobReason.PLACE);
            expect(await link.commands().send("pause")).toMatch(/^err/);
            expect(await link.commands().send("cancel")).toBe("ok");
        });
    });

    it("a pause with the next layer already sent resumes, not lost", async () => {
        await withLink(async (link) => {
            const sess = link.session(new JobSource([
                layerOf(0, 1, Array.from({ length: 20 }, (_, i) => rec(i + 1))),
                layerOf(1, 2, Array.from({ length: 20 }, (_, i) => rec(i + 21))),
            ]));
            const done = sess.run();
            await signOff(link, sess, done);
            await until(async () => (await link.freshStatus()).reason === JobReason.RUNNING);
            expect(await link.commands().send("pause")).toBe("ok");
            await until(() => sess.hold === "paused");
            expect(await sess.resume()).toBe(true);
            await until(() => atX(link, 40));
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { recordMs: 20, ringSize: 30 });
    });
});

describe("wire/link/session: manual layers", () => {
    const twoLayers = () => new JobSource([
        layerOf(0, 1, [rec(1), rec(2)]), layerOf(1, 1, [rec(3), rec(4)]),
    ], { manual: true });

    it("sends each layer only on release()", async () => {
        await withLink(async (link) => {
            const src = twoLayers();
            const sess = link.session(src);
            const done = sess.run();
            await signOff(link, sess, done);
            await until(() => sess.hold === "release");
            await tick(60);
            expect(sess.stats().acked).toBe(src.preflightFrames);

            src.release();
            // `release` only once the layer is cut: never while blocks are queued.
            for (let i = 0; i < 100 && sess.hold !== "release"; i++) {
                await tick(10);
            }
            expect(sess.hold).toBe("release");
            expect(link.status?.bufCount).toBe(0);
            expect((await link.freshStatus()).wpos?.[0]).toBe(2);
            expect(sess.drained).toBe(false);

            src.release();
            await until(() => atX(link, 4));
            expect(sess.drained).toBe(true);
            expect(sess.hold).toBeNull();
            expect(await sess.close()).toBe("ok");
            expect((await done).ok).toBe(true);
        });
    });

    it("clearing manual lets the remaining layers run", async () => {
        await withLink(async (link) => {
            const src = twoLayers();
            const sess = link.session(src);
            const done = sess.run();
            await signOff(link, sess, done);
            src.release();
            await until(() => atX(link, 2));
            src.manual = false;
            await until(() => atX(link, 4));
            expect(await sess.close()).toBe("ok");
            await done;
        });
    });

    it("a resume at a layer boundary keeps the later layers back", async () => {
        const src = twoLayers();
        const first = src.start();
        first.pull(100);
        src.release();
        first.pull(100);
        const out = src.from(0, 2).pull(100)!;
        expect(out.map((p) => p[0])).toEqual([MAGIC_LAYER_HDR]);
        expect(src.waiting).toBe(true);
        src.release();
        expect(src.waiting).toBe(false);
    });

    it("is off by default", async () => {
        const src = new JobSource([layerOf(0, 1, [rec(1)]), layerOf(1, 1, [rec(2)])]);
        const stream = src.start();
        const out = stream.pull(100)!;
        expect(out.length).toBe(src.preflightFrames + 6);
        expect(src.drained).toBe(true);
        expect(src.waiting).toBe(false);
    });
});

describe("wire/link/session: placement", () => {
    const open = async (link: Link) => {
        const sess = link.session(job(3));
        const done = sess.run();
        await until(() => sess.hold === "place");
        return { sess, done };
    };

    it("sends no layer until `place ok`, then runs the job", async () => {
        await withLink(async (link) => {
            const { sess, done } = await open(link);
            await tick(150);
            expect(sess.stats()).toMatchObject({ sent: 2, acked: 2, nacks: 0 });
            expect(sess.hold).toBe("place");
            expect(await sess.resume()).toBe(false);
            expect(await link.commands().send("place ok")).toBe("err bad_state"); // no zero set
            expect(await link.commands().send("place set")).toBe("ok");
            expect(await link.commands().send("place go xmax ymax")).toBe("ok");
            expect(await link.commands().send("place frame")).toBe("ok");
            await tick(150);
            expect(sess.hold).toBe("place");
            expect(await link.commands().send("place ok")).toBe("ok");
            await until(() => atX(link, 3));
            expect(sess.hold).toBeNull();
            await sess.close();
            expect((await done).ok).toBe(true);
        });
    });

    it("a jog holds on, and `place reset` lets it jog again", async () => {
        await withLink(async (link) => {
            await link.commands().send("setorigin");
            const { sess, done } = await open(link);
            expect(await link.commands().send("jog z 1")).toBe("err bad_state");
            expect(await link.commands().send("jog x 5 100")).toMatch(/^ok/);
            expect(await link.commands().send("place set")).toBe("err busy");
            await until(async () => (await link.freshStatus()).state === MachineState.JOB);
            expect(sess.hold).toBe("place");
            expect(await link.commands().send("place set")).toBe("ok");
            expect(await link.commands().send("jog x 1")).toBe("err bad_state");
            expect(await link.commands().send("place reset")).toBe("ok");
            expect(await link.commands().send("jog x 1 100")).toMatch(/^ok/);
            await until(async () => (await link.freshStatus()).state === MachineState.JOB);
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { frameMs: 5, cjogStepsPerS: 1000 });
    });

    it("close() is refused while moving and the hold goes on; abort() stops it", async () => {
        await withLink(async (link) => {
            await link.commands().send("setorigin");
            const { sess, done } = await open(link);
            expect(await link.commands().send("jog x 5000 1")).toMatch(/^ok/);
            expect(await sess.close()).toBe("err bad_state");
            await tick(150);
            expect(sess.hold).toBe("place");
            sess.abort();
            expect((await done).ok).toBe(true);
            expect((await link.freshStatus()).state).toBe(MachineState.IDLE);
        }, { frameMs: 5, cjogStepsPerS: 1000 });
    });

    it("refuses layers and bad usage", async () => {
        await withLink(async (link, sim) => {
            await link.resetSeq();
            await sim.write(stampSeq(packJobHdr({ recordCount: 0, toolCount: 1 }), 0));
            await sim.write(stampSeq(packToolHdr({ tool: 1, head: 0, bbox: BOX }), 1));
            expect(await link.commands().send("place go xmax")).toBe("err usage");
            await link.resetSeq();
            await sim.write(stampSeq(packLayerHdr({ layer: 0 }), 0));
            expect(await link.sinks.ack.get(500)).toEqual(new Refusal(Nack.ORDER, "layer header in place"));
            const st = await link.freshStatus();
            expect([st.state, st.reason]).toEqual([MachineState.JOB, JobReason.PLACE]);
            expect(await link.commands().send("cancel")).toBe("ok");
        });
    });

    it("close() and abort() leave the hold; a cancel behind its back is lost", async () => {
        await withLink(async (link) => {
            const a = await open(link);
            expect(await a.sess.close()).toBe("ok");
            expect((await a.done).ok).toBe(true);

            const b = await open(link);
            b.sess.abort();
            expect((await b.done).ok).toBe(true);

            const c = await open(link);
            expect(await link.commands().send("cancel")).toBe("ok");
            expect((await c.done).fatal).toEqual({ by: "host", why: "job-lost" });
        });
    });

    it("locks XY in TOOL_WAIT, Z alone may be zeroed", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 3);
            await until(async () => (await link.freshStatus()).reason === JobReason.TOOL_WAIT);
            expect(await link.commands().send("wzero x")).toBe("err bad_state");
            expect(await link.commands().send("wzero")).toBe("err bad_state");
            expect(await link.commands().send("wzero z")).toBe("ok");
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { tools: {}, toolChangeMs: 20 });
    });
});

describe("wire/link/session: tool change", () => {
    it("waits in TOOL_WAIT for resume(), and again until the tool is mounted and zeroed", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 3);
            await until(() => sess.hold === "tool");
            await until(async () => (await link.freshStatus()).reason === JobReason.TOOL_WAIT);
            // Nothing is resent without the operator.
            const sent = sess.stats().sent;
            await tick(150);
            expect(sess.stats().sent).toBe(sent);

            expect(await sess.resume()).toBe(false); // not mounted yet: refused again
            await until(() => sess.stats().sent > sent);
            await until(() => sess.hold === "tool");

            expect(await link.commands().send("mount 0 1")).toBe("ok");
            expect(await link.commands().send("wzero z")).toBe("ok");
            sess.resume();
            await until(() => atX(link, 3));
            expect(sess.hold).toBeNull();
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { tools: {} });
    });

    it("zeroes another head after selecting it in TOOL_WAIT", async () => {
        await withLink(async (link, sim) => {
            const { sess, done } = await start(link, 3);
            await until(async () => (await link.freshStatus()).reason === JobReason.TOOL_WAIT);
            expect(await link.commands().send("select 1")).toBe("ok");
            expect(await link.commands().send("wzero z")).toBe("ok");
            expect(sim.zeroed.has(1)).toBe(true);
            expect(sim.zeroed.has(0)).toBe(false);
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { tools: {}, toolChangeMs: 20 });
    });

    it("an unhomed A refused in preflight is fatal", async () => {
        await withLink(async (link, sim) => {
            sim.refuseTool = { tool: 1, code: Nack.NOT_HOMED };
            const { done } = await start(link, 2);
            expect((await done).fatal).toMatchObject({ by: "controller", nack: Nack.NOT_HOMED });
            expect((await link.freshStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("refuses resume() during TOOL_CHANGE, and abort() leaves the hold", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 3);
            await until(() => sess.hold === "tool");
            await until(async () => (await link.freshStatus()).reason === JobReason.TOOL_CHANGE);
            await until(() => sess.hold === "tool");
            expect(await sess.resume()).toBe(false);
            sess.abort();
            expect((await done).ok).toBe(true);
        }, { tools: {}, toolChangeMs: 2000 });
    });

    it("a jog in TOOL_WAIT keeps the hold, and resume() after it resumes", async () => {
        await withLink(async (link, sim) => {
            await link.commands().send("setorigin");
            const { sess, done } = await start(link, 3);
            await until(async () => (await link.freshStatus()).reason === JobReason.TOOL_WAIT);
            const sent = sess.stats().sent;
            await sim.write(packCjog(1, 0));
            await until(async () => (await link.freshStatus()).state === MachineState.JOGGING);
            expect(await sess.resume()).toBe(false);
            for (let t = 0; t < 400; t += 30) { // held past several of the session's polls
                await sim.write(packCjog(1, 0));
                await tick(30);
            }
            await until(async () => (await link.freshStatus()).state === MachineState.JOB, CJOG_DEADMAN_MS + 500);
            expect(sess.hold).toBe("tool");
            expect(sess.stats().sent).toBe(sent); // nothing resent during the jog

            expect(await link.commands().send("mount 0 1")).toBe("ok");
            expect(await link.commands().send("wzero z")).toBe("ok");
            expect(await sess.resume()).toBe(true);
            await until(() => sess.hold === null);
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { tools: {}, toolChangeMs: 20, frameMs: 5, cjogStepsPerS: 1000, cjogTravel: [[0, 400], [0, 400]] });
    });

    it("a mounted, zeroed tool is ACKed with no hold", async () => {
        await withLink(async (link) => {
            const { sess, done } = await start(link, 2);
            await until(() => atX(link, 2));
            expect(sess.stats().nacks).toBe(0);
            await sess.close();
            expect((await done).ok).toBe(true);
        }, { tools: { mounted: { 0: 1 }, zeroed: [0] } });
    });
});
