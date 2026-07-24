/**
 * Tests for wire/link/session — the Go-Back-N windowed stream over a
 * PacketSource. Pins the architectural properties from docs/comms_architecture.md
 * §2.3 that the Python session.test pins (host/diagnostics/test_session.py):
 *
 *   - pull() called AT MOST ONCE PER PACKET, EVER. A go-back replays from the
 *     retained window, never from the source — so under forced backpressure
 *     the source's total pulled equals the packet count, even though the
 *     writer may have sent some packets multiple times.
 *   - termination tests packets EMITTED, not SENT — after a go-back `_next`
 *     rewinds to `_base` while the window still holds pulled-but-unsent
 *     packets, so testing `_next` would strand them and report success early.
 *   - NACK_BAD_MAGIC / NACK_PAUSED / NACK_BAD_STATE are fatal; NACK_FULL is
 *     backpressure (resend from base); truncation is a normal outcome (run
 *     returns true, truncated flag set).
 */

import { describe, it, expect } from "vitest";
import {
    Session,
    ListSource,
    StreamContext,
    type PacketSource,
} from "../../../src/wire/link/session.js";
import { Writer } from "../../../src/wire/link/writer.js";
import { Sink } from "../../../src/wire/link/sink.js";
import { Ack, Nack } from "../../../src/wire/link/demux.js";
import { NACK_FULL, NACK_BAD_MAGIC, PACKET_SIZE } from "../../../src/wire/format/constants.js";
import { packMicrosegment } from "../../../src/wire/format/packet.js";
import { microSegment } from "../../../src/wire/format/microsegment.js";

type AckOrNack = Ack | Nack;

function pkt(seq: number): Uint8Array {
    return packMicrosegment(microSegment(1, 0, 0, 0, 1000), seq);
}

function pkts(n: number): Uint8Array[] {
    return Array.from({ length: n }, (_, i) => pkt(i));
}

/**
 * FakePico: a Writable that consumes a writeBatch's concatenated 26-byte
 * packets, replying with cumulative ACKs on the shared ack sink. Optionally
 * injects a NACK_FULL at a given WRITE index (one-shot — the resend at a new
 * index is accepted), simulating a ring that was full once then drained.
 */
class FakePico {
    readonly ackSink: Sink<AckOrNack>;
    readonly injected: Set<number>;
    expectedSeq = 0;
    sentPackets: Uint8Array[] = [];
    writes: Uint8Array[] = [];

    constructor(ackSink: Sink<AckOrNack>, injectFullAtWriteIdx: number[] = []) {
        this.ackSink = ackSink;
        this.injected = new Set(injectFullAtWriteIdx);
    }

    write(bytes: Uint8Array): Promise<void> {
        this.writes.push(bytes);
        for (let off = 0; off + PACKET_SIZE <= bytes.length; off += PACKET_SIZE) {
            const p = bytes.subarray(off, off + PACKET_SIZE);
            const seq = p[22]!;
            const writeIdx = this.sentPackets.length;
            this.sentPackets.push(p);
            if (this.injected.has(writeIdx)) {
                this.ackSink.put(new Nack(NACK_FULL));
            } else {
                this.expectedSeq = (seq + 1) & 0xff;
                this.ackSink.put(new Ack(this.expectedSeq));
            }
        }
        return Promise.resolve();
    }
}

/** Source that records every productive pull's batch count, to prove the
 *  session never re-pulls a packet (sum of batches === total packets). */
class RecordingSource implements PacketSource {
    private packets: Uint8Array[];
    pullLog: number[] = [];

    constructor(packets: Uint8Array[]) {
        this.packets = [...packets];
    }

    pull(ctx: StreamContext): Promise<Uint8Array[] | null> {
        if (this.packets.length === 0) return Promise.resolve(null);
        if (ctx.room <= 0) return Promise.resolve([]);
        const take = Math.min(ctx.room, this.packets.length);
        const chunk = this.packets.splice(0, take);
        this.pullLog.push(chunk.length);
        return Promise.resolve(chunk);
    }
}

/** Source that never produces and never finishes — for the truncate test. */
class HangingSource implements PacketSource {
    pull(): Promise<Uint8Array[] | null> {
        return Promise.resolve([]);
    }
}

function sessionOf(
    source: PacketSource,
    ackSink: Sink<AckOrNack> = new Sink<AckOrNack>("ack"),
    injectFullAt: number[] = [],
): { sess: Session; pico: FakePico } {
    const pico = new FakePico(ackSink, injectFullAt);
    const writer = new Writer(pico);
    const sess = new Session(writer, ackSink, source, undefined, 4);
    return { sess, pico };
}

describe("wire/link/session: closed happy path", () => {
    it("streams N packets, all ACKed, run() returns true", async () => {
        const { sess, pico } = sessionOf(new RecordingSource(pkts(8)));
        expect(await sess.run()).toBe(true);
        expect(sess.stats()).toMatchObject({
            emitted: 8,
            acked: 8,
            nacks: 0,
            retries: 0,
            truncated: false,
        });
        expect(pico.sentPackets.length).toBe(8); // no re-sends on the happy path
    });

    it("pulls each packet exactly once", async () => {
        const src = new RecordingSource(pkts(8));
        const { sess } = sessionOf(src);
        await sess.run();
        // sum of productive pull batches === 8 (no packet re-pulled)
        expect(src.pullLog.reduce((a, b) => a + b, 0)).toBe(8);
    });

    it("an empty source completes immediately", async () => {
        const { sess } = sessionOf(new RecordingSource([]));
        expect(await sess.run()).toBe(true);
        expect(sess.stats().emitted).toBe(0);
    });
});

describe("wire/link/session: backpressure (NACK_FULL)", () => {
    it("pull once-per-packet holds under forced backpressure", async () => {
        // Inject NACK_FULL at write indices 2 and 6 — force two go-backs.
        const src = new RecordingSource(pkts(12));
        const { sess, pico } = sessionOf(src, undefined, [2, 6]);
        expect(await sess.run()).toBe(true);

        // Each packet pulled exactly once, despite the re-sends.
        expect(src.pullLog.reduce((a, b) => a + b, 0)).toBe(12);
        expect(sess.stats().emitted).toBe(12);
        expect(sess.stats().acked).toBe(12);
        // The writer sent more than 12 (re-sends happened).
        expect(pico.sentPackets.length).toBeGreaterThan(12);
        expect(sess.stats().retries).toBeGreaterThan(0);
        expect(sess.stats().nacks).toBeGreaterThan(0);
    });

    it("terminates on EMITTED not SENT — does not strand pulled-but-unsent packets", async () => {
        // A go-back rewinds `_next` to `_base`; if run() wrongly tested
        // `_next` it would exit while `_emitted > base`, silently truncating.
        // Inject NACK_FULL late, after the window was filled from the source.
        const src = new RecordingSource(pkts(10));
        const { sess } = sessionOf(src, undefined, [7]);
        expect(await sess.run()).toBe(true);
        // All 10 produced packets must be confirmed — not just the ones sent.
        expect(sess.stats().emitted).toBe(10);
        expect(sess.stats().acked).toBe(10);
    });
});

describe("wire/link/session: fatal NACKs", () => {
    it("NACK_BAD_MAGIC is fatal — run() returns false", async () => {
        const ackSink = new Sink<AckOrNack>("ack");
        const pico = new FakePico(ackSink);
        // override: NACK_BAD_MAGIC on the first packet
        const origWrite = pico.write.bind(pico);
        pico.write = (bytes: Uint8Array) => {
            pico.writes.push(bytes);
            for (let off = 0; off + PACKET_SIZE <= bytes.length; off += PACKET_SIZE) {
                pico.sentPackets.push(bytes.subarray(off, off + PACKET_SIZE));
                ackSink.put(new Nack(NACK_BAD_MAGIC));
            }
            return Promise.resolve();
        };
        void origWrite;
        const writer = new Writer(pico);
        const sess = new Session(writer, ackSink, new ListSource(pkts(4)), undefined, 4);
        expect(await sess.run()).toBe(false);
        expect(sess.stats().nacks).toBeGreaterThan(0);
    });
});

describe("wire/link/session: truncation (open session)", () => {
    it("truncate() ends a hanging open session — run() returns true, not false", async () => {
        const { sess } = sessionOf(new HangingSource());
        const runP = sess.run();
        // truncate before or during the idle wait — either way run() exits
        // promptly because the abort promise wakes ctx.wait() and the loop
        // top checks `truncated`.
        sess.truncate();
        expect(await runP).toBe(true);
        expect(sess.truncated).toBe(true);
        expect(sess.stats().truncated).toBe(true);
    });

    it("abortPromise races against ctx.wait — a truncated session wakes immediately", async () => {
        const ackSink = new Sink<AckOrNack>("ack");
        const pico = new FakePico(ackSink);
        const writer = new Writer(pico);
        const sess = new Session(writer, ackSink, new HangingSource(), undefined, 4);
        const runP = sess.run();
        // A source blocked in ctx.wait() should resolve the moment truncate
        // fires the abort promise — no latency floor equal to IDLE_WAIT_MS.
        const t0 = Date.now();
        sess.truncate();
        await runP;
        expect(Date.now() - t0).toBeLessThan(50);
    });
});