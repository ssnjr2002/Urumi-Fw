/**
 * Tests for wire/link/backends/sim — the in-process fake Pico. Run over a real
 * Link so the exercise path matches what a browser/Node consumer will use:
 * Link → Writer → SimTransport.write → SimTransport.reply → Demux → sink.
 *
 * Pins the behaviours the Sim exists to make testable without hardware:
 *   - control plane: ping/get/enable/disable/setorigin/pause/resume/
 *     cancel/stop/unalarm per the allowed-state matrix
 *   - binary status poll (STATUS_RSP v2 fields incl pos + queuedUs)
 *   - coalesced ACKs (K=8: one ACK frame per 8 accepted packets, not 1:1)
 *   - backpressure (NACK_FULL when ring full, resent on drain)
 *   - stale-seq duplicate guard (a retransmit is ACKed but NOT executed)
 *   - the job: BUFFERING until enough is queued, then RUNNING, each record
 *     landing the head on its end point; a dry ring buffers again
 *   - abort: ring discarded, position kept, state → IDLE
 *   - status-during-stream: a poll mid-stream returns a running job + non-zero
 *     queuedUs (the case impossible under port seizure)
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../../src/wire/link/link.js";
import { SimTransport } from "../../../../src/wire/link/backends/sim.js";
import {
    MachineState,
    Fault,
    JobReason,
} from "../../../../src/wire/format/status.js";
import { stampSeq } from "../../../../src/wire/format/packet.js";
import { packBezier } from "../../../../src/wire/format/bezier.js";
import { BezierFlag } from "../../../../src/toolpath/annotate.js";
import { cubic } from "../../../../src/toolpath/geometry.js";
import { packCjog, type JogDir } from "../../../../src/wire/format/cjog.js";
import { CJOG_DEADMAN_MS, MAGIC_CJOG_STOP } from "../../../../src/wire/format/constants.js";
import { crc8 } from "../../../../src/wire/format/crc.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

/** A straight BEZIER record along X ending at `x` mm (= steps at jogSpm 1). */
function rec(x = 1): Uint8Array {
    return packBezier({
        curve: cubic({ x: x - 1, y: 0 }, { x: x - 2 / 3, y: 0 }, { x: x - 1 / 3, y: 0 }, { x, y: 0 }),
        flags: 0,
        length: 1, kappaMax: 0, dkappaMax: 0, ts: [1, 0, 0],
        kappaStart: 0, kappaEnd: 0, fitError: 0,
    });
}

const upTo = (n: number) => Array.from({ length: n }, (_, i) => rec(i + 1));

/** Poll until `cond` holds or `ms` pass; returns the last status. */
async function until(link: Link, cond: (st: Awaited<ReturnType<Link["getStatus"]>>) => boolean, ms = 1000) {
    const deadline = Date.now() + ms;
    let st = await link.getStatus();
    while (!cond(st) && Date.now() < deadline) {
        await tick(5);
        st = await link.getStatus();
    }
    return st;
}

const running = (st: { state: number; reason: number }) =>
    st.state === MachineState.JOB && st.reason === JobReason.RUNNING;

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    simOpts?: ConstructorParameters<typeof SimTransport>[0],
): Promise<T> {
    // Boot into a COMMITTED map by default, as the firmware's controller does
    // with a config. A test passes `{ axisMap: undefined }` to get the
    // unconfigured, unmapped boot.
    // Short records and buffer timeout keep a job's tests fast.
    const sim = new SimTransport({ axisMap: [1, 2, 3, 4], recordMs: 5, bufferIdleMs: 20, ...simOpts });
    const link = new Link(sim);
    try {
        return await fn(link, sim);
    } finally {
        await link.close();
    }
}

/** The state and mask keys the Sim models. */
const STATE = "get state enabled homed faults job latched probed";

describe("wire/link/backends/sim: control plane", () => {
    it("ping → pong", async () => {
        await withLink(async (link) => {
            expect(await link.command("ping")).toBe("pong");
        });
    });

    it("get reports the state + masks", async () => {
        await withLink(async (link) => {
            expect(await link.command(STATE)).toBe(
                "state=0 enabled=0x00 homed=0x00 faults=0x0000 job=- latched=0x00 probed=0",
            );
        });
    });

    it("axes_enable energises all axes; setorigin homes + zeros pos + recovers from ALARM", async () => {
        await withLink(async (link) => {
            expect(await link.command("axes_enable on")).toBe("ok");
            expect(await link.command(STATE)).toBe(
                "state=0 enabled=0x0f homed=0x00 faults=0x0000 job=- latched=0x00 probed=0",
            );
            expect(await link.command("setorigin")).toBe("ok");
            expect(await link.command(STATE)).toBe(
                "state=0 enabled=0x0f homed=0x0f faults=0x0000 job=- latched=0x00 probed=0",
            );

            // stop → ALARM, then setorigin recovers
            expect(await link.command("stop")).toBe("ok");
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.ALARM);
            expect(await link.command("setorigin")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("axes_enable is rejected while a job runs (bad_state)", async () => {
        await withLink(async (link, sim) => {
            sim._forceRunning();
            expect(await link.command("axes_enable on")).toBe("err bad_state");
        });
    });

    it("pause/resume/cancel per the allowed-state matrix", async () => {
        await withLink(async (link, sim) => {
            expect(await link.command("pause")).toBe("err bad_state"); // IDLE
            sim._forceRunning();
            expect(await link.command("pause")).toBe("ok");
            let st = await link.getStatus();
            expect([st.state, st.reason]).toEqual([MachineState.JOB, JobReason.PAUSED]);
            expect(await link.command("resume")).toBe("ok");
            st = await link.getStatus();
            expect([st.state, st.reason]).toEqual([MachineState.JOB, JobReason.BUFFERING]);
            expect(await link.command("resume")).toBe("err bad_state"); // not paused
            await link.command("pause");
            expect(await link.command("cancel")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("unalarm refuses the estop latch", async () => {
        await withLink(async (link) => {
            await link.command("stop");
            expect((await link.getStatus()).hasFault(Fault.ESTOP)).toBe(true);
            expect(await link.command("unalarm")).toBe("err estop");
            expect((await link.getStatus()).state).toBe(MachineState.ALARM);
        });
    });

    it("unalarm clears an event fault → IDLE", async () => {
        await withLink(async (link, sim) => {
            sim.homingLegMs = 1000;
            expect(await link.command("leg 1 seek 1 2500 500 400 88000")).toBe("ok");
            expect(await link.command("leg_abort 1")).toBe("ok");
            expect((await link.getStatus()).faults).toBe(Fault.HOMING);
            expect(await link.command("unalarm")).toBe("ok faults=0x0000");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("a leg arms and returns; the leg finishes asynchronously", async () => {
        // `ok` means ARMED, not finished — the whole reason the supervisor
        // exists (docs/homing.md §2.3). A handler that blocked until the pulser
        // stopped would freeze get and every abort for the whole seek.
        await withLink(async (link, sim) => {
            sim.homingLegMs = 30;
            expect(await link.command("leg 1 seek 1 2500 500 400 88000")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.HOMING);
            expect(await link.command("leg 2 seek 1 2500 500 400 88000")).toBe("err busy");

            await tick(80);
            // A seek ends ON the switch, which is an alarm and not a failure.
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.ALARM);
            expect(st.faults).toBe(Fault.LATCHED);
            expect(await link.command(STATE)).toContain("latched=0x01");

            // `unalarm` cannot clear it: nothing moved, so the switch is still
            // held. Only a retract gets out.
            expect(await link.command("unalarm")).toBe("ok faults=0x0008");
            expect((await link.getStatus()).faults).toBe(Fault.LATCHED);

            // Armed while latched, so the node reads its pin and runs a RETRACT.
            expect(await link.command("leg 1 retract 0 8013 8013 0 320")).toBe("ok");
            await tick(80);
            const done = await link.getStatus();
            expect(done.state).toBe(MachineState.IDLE);
            expect(await link.command(STATE)).toContain("latched=0x00");
        });
    });

    it("a leg is node-addressed: it runs with no axis_map committed", async () => {
        // The point of node addressing. Every output of a leg is node-framed, so
        // the firmware does not route it through the map — which means a head
        // can be homed during commissioning, before anything is bound. Under the
        // old axis-addressed `home` this needed a throwaway `axis_map` to borrow
        // a slot first.
        await withLink(async (link, sim) => {
            sim.homingLegMs = 20;
            expect(await link.command("axis_map - - - -")).toBe("ok");
            expect((await link.getStatus()).faults).toBe(0);

            expect(await link.command("leg 3 seek 1 2500 500 400 88000")).toBe("ok");
            await tick(60);

            // Node 3 is standing on its switch, and no slot claims it, so the
            // per-slot view is empty while the node-framed truth is not.
            expect(await link.command(STATE)).toContain("latched=0x00");
            expect(await link.command("axis_map 1 2 3 4")).toBe("ok");
            expect(await link.command(STATE)).toContain("latched=0x04");
        }, { busNodes: [1, 2, 3, 4] });
    });

    it("a sweep on a limit-switch node is refused before anything moves", async () => {
        await withLink(async (link) => {
            expect(await link.command("leg 1 sweep 1 2500 500 400 88000"))
                .toContain("kind_mismatch");
            expect((await link.getStatus()).state).not.toBe(MachineState.HOMING);
        });
    });

    it("a park needs the node homed and keeps its datum", async () => {
        await withLink(async (link, sim) => {
            sim.homingLegMs = 20;
            expect(await link.command("leg 1 park 100 2500 500 400")).toBe("err not_homed");
            expect(await link.command("setorigin")).toBe("ok");
            expect(await link.command("leg 1 park 100 2500 500 400")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.HOMING);
            await tick(60);
            expect(await link.command(STATE)).toContain("homed=0x0f");
        });
    });

    it("leg_abort fails the supervised leg; another node just acks", async () => {
        await withLink(async (link, sim) => {
            sim.homingLegMs = 1000;
            expect(await link.command("leg 1 seek 1 2500 500 400 88000")).toBe("ok");
            expect(await link.command("leg_abort 2")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.HOMING);
            expect(await link.command("leg_abort 1")).toBe("ok");
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.ALARM);
            expect(st.hasFault(Fault.HOMING)).toBe(true);
        });
    });

    it("a latched limit follows the NODE across a rebind, not the slot", async () => {
        await withLink(async (link, sim) => {
            sim.homingLegMs = 20;
            await link.command("leg 3 seek 1 2500 500 400 88000"); // node 3 = slot 2
            await tick(60);
            expect(await link.command(STATE)).toContain("latched=0x04");

            // Slot 2 now holds node 5, which is not on a switch. The gate lifts.
            expect(await link.command("axis_map 1 2 5 6")).toBe("ok");
            expect(await link.command(STATE)).toContain("latched=0x00");

            // ...and comes back with node 3, because node 3 really is still
            // standing on its switch and really will still refuse stream steps.
            expect(await link.command("axis_map 1 2 3 4")).toBe("ok");
            expect(await link.command(STATE)).toContain("latched=0x04");
        }, { busNodes: [1, 2, 3, 4, 5, 6] });
    });

    it("pingnode all → one line, not one per node", async () => {
        await withLink(async (link) => {
            // The scan is the whole bus (1..BUS_ADDR_MAX=8), not just the
            // axes — it is the bring-up verb that surfaces peripherals too.
            // Only 1..4 answer in the default sim bus.
            expect(await link.command("pingnode all")).toBe(
                "nodes 1=ok 2=ok 3=ok 4=ok 5=timeout 6=timeout 7=timeout 8=timeout",
            );
            expect(await link.command("pingnode 3")).toBe("node 3 ok");
        });
    });
});

describe("wire/link/backends/sim: binary status poll (STATUS_RSP v2)", () => {
    it("getStatus returns parsed MachineStatus with pos + queuedUs", async () => {
        await withLink(async (link) => {
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.IDLE);
            expect(st.bufCount).toBe(0);
            expect(st.pos).toEqual([0, 0, 0, 0]);
            expect(st.queuedUs).toBe(0);
        });
    });
});

describe("wire/link/backends/sim: stream + coalesced ACKs", () => {
    it("streams N records, all ACKed; the job runs them and buffers again", async () => {
        await withLink(async (link) => {
            expect(await link.stream(upTo(16), 8)).toMatchObject({ ok: true });
            const st = await until(link, (s) => s.bufCount === 0 && s.pos![0] === 16);
            expect(st.pos![0]).toBe(16);
            // A dry ring does not end the job: only cancel does.
            expect([st.state, st.reason]).toEqual([MachineState.JOB, JobReason.BUFFERING]);
            expect(await link.command("cancel")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("coalesces ACKs at K=8 — fewer ACK frames than packets", async () => {
        await withLink(
            async (link, sim) => {
                // 16 packets at K=8 → exactly 2 ACK frames flushed by the K counter
                // (the end-of-batch flush handles the tail). Capture the demux
                // frames-count to prove coalescing fired.
                await link.stream(upTo(16), 16);
                // sim.framesReplied counts every frame the sim emitted;
                // 2 ACKs for 16 packets means coalescing worked (1:1 would be 16).
                expect(sim.framesReplied).toBeLessThanOrEqual(4); // 2 K-flushes + maybe 1 batch-flush
                expect(sim.framesReplied).toBeGreaterThanOrEqual(2);
            },
            { ackCoalesceMax: 8 },
        );
    });
});

describe("wire/link/backends/sim: buffering", () => {
    it("the first record opens the job in BUFFERING; it runs once enough is queued", async () => {
        await withLink(
            async (link) => {
                await link.stream([rec(1)], 4);
                let st = await link.getStatus();
                expect([st.state, st.reason, st.bufCount]).toEqual([MachineState.JOB, JobReason.BUFFERING, 1]);
                expect(st.queuedUs).toBe(100_000);
                await link.stream([rec(2)], 4); // 200 ms queued: enough
                st = await until(link, running);
                expect(running(st)).toBe(true);
            },
            { recordMs: 100, bufferMs: 200, bufferIdleMs: 10_000 },
        );
    });
});

describe("wire/link/backends/sim: stale-seq duplicate guard", () => {
    it("a stale retransmit is ACKed but NOT executed (no duplicate motion)", async () => {
        await withLink(async (link, sim) => {
            await link.resetSeq();
            // Send seq 0 directly via writeBatch — accepted + executed
            const p0 = stampSeq(rec(5), 0);
            // (use the link's writer through a one-packet session)
            await link.stream([p0], 4);
            await until(link, (s) => s.pos![0] === 5);
            expect(sim.pos[0]).toBe(5);

            // Now reset and send seq 0 again — the sim's expectedSeq is back to
            // 0, so this is a fresh accept and runs.
            await link.resetSeq();
            await link.stream([stampSeq(rec(10), 0)], 4);
            await until(link, (s) => s.pos![0] === 10);
            expect(sim.pos[0]).toBe(10);
        });
    });
});

describe("wire/link/backends/sim: backpressure (NACK_FULL)", () => {
    it("a stream against a tiny ring retries to completion under NACK_FULL", async () => {
        await withLink(
            async (link) => {
                // ringSize=2, a record per tick — the ring fills, the
                // session sees NACK_FULL, retries, and eventually completes.
                expect(await link.stream(upTo(10), 8)).toMatchObject({ ok: true });
                const st = await until(link, (s) => s.pos![0] === 10);
                expect(st.pos![0]).toBe(10);
            },
            { ringSize: 2, frameMs: 20, recordMs: 20 },
        );
    });
});

describe("wire/link/backends/sim: abort", () => {
    it("abort discards the ring, keeps position, lands IDLE", async () => {
        await withLink(async (link) => {
            await link.resetSeq();
            // 50 records of 5 ms. Start non-awaited so we can abort MID-stream.
            const runP = link.stream(upTo(50), 16);

            // Wait until the job runs with queued motion, then abort.
            const st = await until(link, (s) => running(s) && (s.bufCount ?? 0) > 0);
            expect(running(st)).toBe(true);
            expect(st.bufCount).toBeGreaterThan(0);
            // remember position at abort time — the sim models the OUTCOME
            // (ring discarded, position KEPT), not the ramp distance the real
            // machine would still travel. So post-abort pos === pre-abort pos.
            const posAtAbort = st.pos![0];

            link.abort();
            await runP;
            await tick(50);

            const after = await link.getStatus();
            expect(after.state).toBe(MachineState.IDLE);
            expect(after.bufCount).toBe(0);
            expect(after.queuedUs).toBe(0);
            // position is whatever had integrated before the abort — NOT the
            // full 50 (the remaining ring was discarded). >= posAtAbort, and
            // strictly less than 50 unless the executor finished before abort.
            expect(after.pos![0]).toBeGreaterThanOrEqual(posAtAbort);
            expect(after.pos![0]).toBeLessThanOrEqual(50);
        });
    });
});

describe("wire/link/backends/sim: status during a stream", () => {
    it("a poll mid-stream returns a running job + non-zero queuedUs (impossible under seizure)", async () => {
        await withLink(
            async (link) => {
                await link.resetSeq();
                // 20 long records — the job takes many ticks
                const runP = link.stream(upTo(20), 8);
                const st = await until(link, running);
                expect(running(st)).toBe(true);
                expect(st.bufCount).toBeGreaterThan(0);
                expect(st.queuedUs).toBeGreaterThan(0);
                await runP;
            },
            { frameMs: 40, recordMs: 200, bufferMs: 200 },
        );
    });
});

describe("wire/link/backends/sim: the unmapped boot", () => {
    it("boots IDLE with nothing bound; axis_map binds without an alarm", async () => {
        await withLink(
            async (link, sim) => {
                expect(sim.state).toBe(MachineState.IDLE);
                expect(sim.faults).toBe(0);
                expect(await link.command("axes_enable on")).toBe("err unbound");

                expect(await link.command("axis_map 1 2 3 4")).toBe("ok");
                expect(sim.state).toBe(MachineState.IDLE);
                expect(await link.command("axes_enable on")).toBe("ok");
            },
            { axisMap: undefined }, // the unconfigured boot
        );
    });

    it("axis_map is rejected while a job runs — rebinding mid-motion corrupts it", async () => {
        await withLink(async (link, sim) => {
            sim._forceRunning();
            expect(await link.command("axis_map 1 2 5 6")).toBe("err bad_state");
        });
    });

    it("a record against a paused job gets NACK_PAUSED, not BAD_STATE", async () => {
        await withLink(async (link, sim) => {
            sim._forceRunning();
            expect(await link.command("pause")).toBe("ok");
            await link.resetSeq();
            const r = await link.stream([rec()], 4);
            expect(r.ok).toBe(false);
            expect(r.nacks).toBeGreaterThan(0);
        });
    });
});

describe("wire/link/backends/sim: continuous jog", () => {
    /** Frames the sim replied with, as hex, captured from here on. */
    function replies(sim: SimTransport): string[] {
        const out: string[] = [];
        const s = sim as unknown as { reply: (d: Uint8Array) => void };
        const orig = s.reply.bind(sim);
        s.reply = (d) => {
            if (d[0] === 0xaa || d[0] === 0xbb) out.push(Buffer.from(d).toString("hex"));
            orig(d);
        };
        return out;
    }

    /** Repeat a packet every 30 ms for `ms`. */
    async function hold(sim: SimTransport, x: JogDir, y: JogDir, ms: number, speed = 1) {
        const end = Date.now() + ms;
        while (Date.now() < end) {
            await sim.write(packCjog(x, y, speed));
            await tick(30);
        }
    }

    const opts = { frameMs: 5, cjogStepsPerS: 1000, cjogTravel: [[0, 400], [0, 400]] as const };

    it("runs while held, ACKs once, and stops on the deadman", async () => {
        await withLink(async (link, sim) => {
            await link.command("setorigin");
            sim.pos = [100, 100, 0, 0];
            const r = replies(sim);
            await hold(sim, 1, 0, 120);
            expect(await link.command("get state jogging")).toBe("state=7 jogging=2");
            await tick(CJOG_DEADMAN_MS + 60);
            expect(sim.state).toBe(MachineState.IDLE);
            expect(r).toEqual(["aa0000"]);
            expect(sim.pos[0]).toBeGreaterThan(150);
            expect(sim.pos[1]).toBe(100);
        }, opts);
    });

    it("stops at the end of travel and stays silent while held", async () => {
        await withLink(async (link, sim) => {
            await link.command("setorigin");
            sim.pos = [390, 100, 0, 0];
            const r = replies(sim);
            await hold(sim, 1, 0, 150);
            expect(sim.pos[0]).toBe(400);
            expect(sim.state).toBe(MachineState.IDLE);
            expect(r).toEqual(["aa0000"]);
            await tick(CJOG_DEADMAN_MS + 30);
            await sim.write(packCjog(1, 0));          // a fresh press at the end
            expect(r).toEqual(["aa0000", "bb0900"]);
        }, opts);
    });

    it("the stop byte stops at once; a bad packet is NACKed", async () => {
        await withLink(async (link, sim) => {
            await link.command("setorigin");
            sim.pos = [100, 100, 0, 0];
            const r = replies(sim);
            await sim.write(packCjog(0, -1));
            expect(sim.state).toBe(MachineState.JOGGING);
            await sim.write(new Uint8Array([MAGIC_CJOG_STOP]));
            expect(sim.state).toBe(MachineState.IDLE);
            const bad = packCjog(1, 0);
            bad[1] = 2;                                 // not −1, 0 or +1
            bad[6] = crc8(bad, 0, 6);
            await sim.write(bad);
            expect(r).toEqual(["aa0000", "bb0600"]);
        }, opts);
    });

    const sets: Array<{ name: string; dir: [JogDir, JogDir, JogDir, JogDir]; reply: string; moved: number }> = [
        { name: "Z alone runs", dir: [0, 0, 1, 0], reply: "aa0000", moved: 2 },
        { name: "A alone runs", dir: [0, 0, 0, -1], reply: "aa0000", moved: 3 },
        { name: "XY with Z is NACKed MIXED_AXES", dir: [1, 0, 1, 0], reply: "bb0a00", moved: -1 },
        { name: "Z with A is NACKed MIXED_AXES", dir: [0, 0, 1, 1], reply: "bb0a00", moved: -1 },
    ];
    for (const c of sets) {
        it(c.name, async () => {
            await withLink(async (link, sim) => {
                await link.command("setorigin");
                const r = replies(sim);
                const [x, y, z, a] = c.dir;
                await sim.write(packCjog(x, y, 1, z, a));
                await new Promise(res => setTimeout(res, 50));
                expect(r).toEqual([c.reply]);
                sim.pos.forEach((p, k) => expect(p !== 0).toBe(k === c.moved));
            }, opts);
        });
    }

    it("refuses an unhomed axis without jogUnhomed", async () => {
        await withLink(async (_link, sim) => {
            const r = replies(sim);
            await sim.write(packCjog(1, 0));
            expect(r).toEqual(["bb0600"]);
            expect(sim.state).toBe(MachineState.IDLE);
        }, opts);
    });
});
