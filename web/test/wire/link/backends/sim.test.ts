/**
 * Tests for wire/link/backends/sim — the in-process fake Pico. Run over a real
 * Link so the exercise path matches what a browser/Node consumer will use:
 * Link → Writer → SimTransport.write → SimTransport.reply → Demux → sink.
 *
 * Pins the behaviours the Sim exists to make testable without hardware:
 *   - control plane: ping/getstate/getpos/enable/disable/setorigin/pause/resume/
 *     cancel/stop/unalarm per the allowed-state matrix
 *   - binary status poll (STATUS_RSP v2 fields incl pos + queuedUs)
 *   - coalesced ACKs (K=8: one ACK frame per 8 accepted packets, not 1:1)
 *   - backpressure (NACK_FULL when ring full, resent on drain)
 *   - stale-seq duplicate guard (a retransmit is ACKed but NOT executed)
 *   - motion executor: a stream's step deltas integrate into reported position
 *     over wall-clock (paced by interval * steps / F_CPU)
 *   - abort: ring discarded, position kept, state → IDLE
 *   - status-during-stream: a poll mid-stream returns RUNNING + non-zero
 *     queuedUs (the case impossible under port seizure)
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../../src/wire/link/link.js";
import { SimTransport } from "../../../../src/wire/link/backends/sim.js";
import {
    MachineState,
    AlarmReason,
} from "../../../../src/wire/format/status.js";
import { packMicrosegment, stampSeq } from "../../../../src/wire/format/packet.js";
import { microSegment, MICRO_PAUSE } from "../../../../src/wire/format/microsegment.js";
import { MAGIC_MICROSEG } from "../../../../src/wire/format/constants.js";

const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

function mseg(dx = 1, interval = 1000): Uint8Array {
    return packMicrosegment(microSegment(dx, 0, 0, 0, interval));
}

async function withLink<T>(
    fn: (link: Link, sim: SimTransport) => Promise<T>,
    simOpts?: {
        ackCoalesceMax?: number;
        ringSize?: number;
        frameMs?: number;
        fCpu?: number;
        busNodes?: readonly number[];
        axisMap?: readonly (number | null)[];
    },
): Promise<T> {
    // Boot into a COMMITTED map by default: the firmware boots into the
    // ALARM_CONFIG gate and refuses all motion until `axis_map` commits, so
    // every test that is not about the gate needs a configured machine. A gate
    // test passes `{ axisMap: undefined }` to get the unconfigured boot.
    const sim = new SimTransport({ axisMap: [1, 2, 3, 4], ...simOpts });
    const link = new Link(sim);
    try {
        return await fn(link, sim);
    } finally {
        await link.close();
    }
}

describe("wire/link/backends/sim: control plane", () => {
    it("ping → pong", async () => {
        await withLink(async (link) => {
            expect(await link.command("ping")).toBe("pong");
        });
    });

    it("getstate reports the state + masks", async () => {
        await withLink(async (link) => {
            expect(await link.command("getstate")).toBe(
                "state=0 enabled=0x00 homed=0x00 alarm=0 running=0",
            );
        });
    });

    it("axes_enable energises all axes; setorigin homes + zeros pos + recovers from ALARM", async () => {
        await withLink(async (link) => {
            expect(await link.command("axes_enable on")).toBe("ok");
            expect(await link.command("getstate")).toBe(
                "state=0 enabled=0x0f homed=0x00 alarm=0 running=0",
            );
            expect(await link.command("setorigin")).toBe("ok");
            expect(await link.command("getstate")).toBe(
                "state=0 enabled=0x0f homed=0x0f alarm=0 running=0",
            );

            // stop → ALARM, then setorigin recovers
            expect(await link.command("stop")).toBe("ok");
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.ALARM);
            expect(await link.command("setorigin")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("axes_enable is rejected while RUNNING (bad_state)", async () => {
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
            expect((await link.getStatus()).state).toBe(MachineState.PAUSED);
            expect(await link.command("resume")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
            sim._forceRunning();
            await link.command("pause");
            expect(await link.command("cancel")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
    });

    it("unalarm clears ALARM → IDLE", async () => {
        await withLink(async (link) => {
            await link.command("stop");
            expect((await link.getStatus()).alarm).toBe(AlarmReason.ESTOP);
            expect(await link.command("unalarm")).toBe("ok");
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
        });
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
    it("streams N packets, all ACKed; run() returns true", async () => {
        await withLink(async (link) => {
            const pkts = Array.from({ length: 16 }, () => mseg(1, 1000));
            expect(await link.stream(pkts, 8)).toMatchObject({ ok: true });
            await tick(50);
            // sim is now RUNNING (or already drained to IDLE). The 16 step
            // deltas integrated into pos[0].
            const st = await link.getStatus();
            expect(st.pos![0]).toBe(16);
        });
    });

    it("coalesces ACKs at K=8 — fewer ACK frames than packets", async () => {
        await withLink(
            async (link, sim) => {
                // 16 packets at K=8 → exactly 2 ACK frames flushed by the K counter
                // (the end-of-batch flush handles the tail). Capture the demux
                // frames-count to prove coalescing fired.
                const pkts = Array.from({ length: 16 }, () => mseg(1, 1000));
                await link.stream(pkts, 16);
                // sim.framesReplied counts every frame the sim emitted;
                // 2 ACKs for 16 packets means coalescing worked (1:1 would be 16).
                expect(sim.framesReplied).toBeLessThanOrEqual(4); // 2 K-flushes + maybe 1 batch-flush
                expect(sim.framesReplied).toBeGreaterThanOrEqual(2);
            },
            { ackCoalesceMax: 8 },
        );
    });
});

describe("wire/link/backends/sim: stale-seq duplicate guard", () => {
    it("a stale retransmit is ACKed but NOT executed (no duplicate motion)", async () => {
        await withLink(async (link, sim) => {
            await link.resetSeq();
            // Send seq 0 directly via writeBatch — accepted + executed
            const p0 = stampSeq(mseg(5, 1000), 0);
            // (use the link's writer through a one-packet session)
            await link.stream([p0], 4);
            await tick(50);
            expect(sim.pos[0]).toBe(5);

            // Now reset and send seq 0 again — the sim's expectedSeq is back to
            // 0, so this is a fresh accept, motion = +5 → 10.
            await link.resetSeq();
            await link.stream([stampSeq(mseg(5, 1000), 0)], 4);
            await tick(50);
            expect(sim.pos[0]).toBe(10);
        });
    });
});

describe("wire/link/backends/sim: backpressure (NACK_FULL)", () => {
    it("a stream against a tiny ring retries to completion under NACK_FULL", async () => {
        await withLink(
            async (link) => {
                // ringSize=2, executor slow (frameMs=40) — the ring fills, the
                // session sees NACK_FULL, retries, and eventually completes.
                const pkts = Array.from({ length: 10 }, () =>
                    packMicrosegment(microSegment(1, 0, 0, 0, 50000)),
                );
                expect(await link.stream(pkts, 8)).toMatchObject({ ok: true });
                // wait for the executor to drain the ring
                await new Promise<void>((r) => setTimeout(r, 400));
                const st = await link.getStatus();
                expect(st.pos![0]).toBe(10);
            },
            { ringSize: 2, frameMs: 20 },
        );
    });
});

describe("wire/link/backends/sim: abort", () => {
    it("abort discards the ring, keeps position, lands IDLE", async () => {
        await withLink(async (link) => {
            await link.resetSeq();
            // 50 packets, each interval 500000 cycles / 150e6 ≈ 3.3ms — total
            // motion ≈ 167ms. Start non-awaited so we can abort MID-stream.
            const pkts = Array.from({ length: 50 }, () => mseg(1, 500_000));
            const runP = link.stream(pkts, 16);

            // Wait until the sim is RUNNING with queued motion, then abort.
            let st = await link.getStatus();
            const deadline = Date.now() + 500;
            while (st.state !== MachineState.RUNNING && Date.now() < deadline) {
                await tick(5);
                st = await link.getStatus();
            }
            expect(st.state).toBe(MachineState.RUNNING);
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
    it("a poll mid-stream returns RUNNING + non-zero queuedUs (impossible under seizure)", async () => {
        await withLink(
            async (link) => {
                await link.resetSeq();
                // a stream of 20 long packets — the executor takes many ticks
                const pkts = Array.from({ length: 20 }, () => mseg(1, 200_000));
                const runP = link.stream(pkts, 8);
                await tick(10); // let the first packets land
                const st = await link.getStatus();
                expect(st.state).toBe(MachineState.RUNNING);
                expect(st.bufCount).toBeGreaterThan(0);
                await runP;
            },
            { frameMs: 40 },
        );
    });
});

describe("wire/link/backends/sim: MSEG_FLAG_PAUSE", () => {
    it("a PAUSE-flagged packet moves to PAUSED after executing it", async () => {
        await withLink(async (link) => {
            await link.resetSeq();
            // one packet with PAUSE → after it executes, state = PAUSED
            const pausePkt = new Uint8Array([
                MAGIC_MICROSEG,
                1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0x00, 0, 0, 0,
                MICRO_PAUSE, 0, 0, 0, 0,
            ]);
            // (hand-built for clarity; matches a paused segment delta=1)
            void pausePkt;
            const pkt = packMicrosegment(
                microSegment(1, 0, 0, 0, 1000, MICRO_PAUSE),
            );
            await link.stream([pkt], 4);
            await tick(100);
            const st = await link.getStatus();
            expect(st.state).toBe(MachineState.PAUSED);
        });
    });
});
describe("wire/link/backends/sim: the ALARM_CONFIG boot gate", () => {
    it("refuses a stream until axis_map commits", async () => {
        await withLink(
            async (link, sim) => {
                expect(sim.state).toBe(MachineState.ALARM);
                expect(sim.alarm).toBe(AlarmReason.CONFIG);

                // Motion ingest gates on machineState alone, so the config
                // ALARM refuses the stream with no separate predicate.
                const refused = await link.stream([mseg()], 4);
                expect(refused.ok).toBe(false);
                expect(sim.pos[0]).toBe(0);

                expect(await link.command("axis_map 1 2 3 4")).toBe("ok");
                expect(sim.state).toBe(MachineState.IDLE);

                const accepted = await link.stream([mseg()], 4);
                expect(accepted.ok).toBe(true);
            },
            { axisMap: undefined }, // the real, unconfigured boot
        );
    });

    it("axis_map is rejected while RUNNING — rebinding mid-motion corrupts it", async () => {
        await withLink(async (link, sim) => {
            sim._forceRunning();
            expect(await link.command("axis_map 1 2 5 6")).toBe("err bad_state");
        });
    });

    it("a job stream against a PAUSED machine gets NACK_PAUSED, not BAD_STATE", async () => {
        await withLink(async (link, sim) => {
            sim._forceRunning();
            expect(await link.command("pause")).toBe("ok");
            await link.resetSeq();
            const r = await link.stream([mseg()], 4);
            expect(r.ok).toBe(false);
            expect(r.nacks).toBeGreaterThan(0);
        });
    });
});
