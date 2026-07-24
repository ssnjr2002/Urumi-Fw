/**
 * Tests for wire/link/link — the Link that owns a transport, demux, writer and
 * sink set. Covers the control-plane surface (command one-outstanding,
 * getStatus binary poll, resetSeq, abort, send, status getter) plus the
 * textDesyncs diagnostic, over a FakeTransport that the test feeds replies
 * into. The real backends (Sim, WebSerial, Node) live in their own commits;
 * this isolates the Link's own behaviour from any specific pipe.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../../src/wire/link/link.js";
import type { Transport } from "../../../src/wire/link/transport.js";
import {
    packStatusRsp,
    MachineState,
    AlarmReason,
    RunningReason,
} from "../../../src/wire/format/status.js";
import {
    MAGIC_ACK,
    MAGIC_ABORT,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
} from "../../../src/wire/format/constants.js";

/** A Transport test double: records writes and lets the test inject reply
 *  bytes that the Link's read loop pumps into the demux. */
class FakeTransport implements Transport {
    writes: Uint8Array[] = [];
    private chunks: Uint8Array[] = [];
    private waiter: (() => void) | null = null;
    private done = false;

    write(bytes: Uint8Array): Promise<void> {
        this.writes.push(bytes);
        return Promise.resolve();
    }

    async *read(): AsyncIterable<Uint8Array> {
        for (;;) {
            if (this.chunks.length > 0) {
                yield this.chunks.shift()!;
            } else if (this.done) {
                return;
            } else {
                await new Promise<void>((r) => {
                    this.waiter = r;
                });
            }
        }
    }

    feedReply(bytes: Uint8Array): void {
        this.chunks.push(bytes);
        const w = this.waiter;
        if (w) {
            this.waiter = null;
            w();
        }
    }

    close(): Promise<void> {
        this.done = true;
        const w = this.waiter;
        if (w) {
            this.waiter = null;
            w();
        }
        return Promise.resolve();
    }
}

const enc = new TextEncoder();
const dec = new TextDecoder();
const tick = (ms = 5) => new Promise<void>((r) => setTimeout(r, ms));

function writtenStrings(t: FakeTransport): string[] {
    return t.writes.map((w) => dec.decode(w));
}

describe("wire/link/link: command (one-outstanding text plane)", () => {
    it("writes the line and returns the reply, stripped", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.command("ping");
        await tick();
        expect(writtenStrings(t)).toEqual(["ping\n"]);
        t.feedReply(enc.encode("pong\n"));
        expect(await p).toBe("pong");
    });

    it("strips trailing \\r from the reply", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.command("ping");
        await tick();
        t.feedReply(enc.encode("pong\r\n"));
        expect(await p).toBe("pong");
    });

    it("returns '' on timeout", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        expect(await link.command("ping", 30)).toBe("");
    });

    it("serializes concurrent commands (one-outstanding)", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p1 = link.command("ping");
        const p2 = link.command("getpos");
        await tick();
        // p1 holds the text lock awaiting "pong"; p2 has NOT written yet.
        expect(writtenStrings(t).filter((s) => s === "ping\n").length).toBe(1);
        expect(writtenStrings(t).some((s) => s.startsWith("getpos"))).toBe(false);

        t.feedReply(enc.encode("pong\n"));
        expect(await p1).toBe("pong");

        // now p2 acquires the lock and writes
        await tick();
        expect(writtenStrings(t).filter((s) => s === "getpos\n").length).toBe(1);
        t.feedReply(enc.encode("pos 0 0 0 0\n"));
        expect(await p2).toBe("pos 0 0 0 0");
    });

    it("textDesyncs counts orphan lines drained before a command", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        // An orphan text line lands with no awaiter — a previous command's
        // multi-line reply tail, the contract breach D11 makes loud.
        t.feedReply(enc.encode("stale\n"));
        await tick(10);
        expect(link.textDesyncs).toBe(0); // not counted yet — nothing drained

        const p = link.command("ping");
        await tick();
        t.feedReply(enc.encode("pong\n"));
        expect(await p).toBe("pong");
        expect(link.textDesyncs).toBe(1); // the orphan was drained by command()
    });
});

describe("wire/link/link: getStatus (binary status plane)", () => {
    it("writes STATUS_REQ and returns a parsed MachineStatus", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.getStatus();
        await tick();
        expect(t.writes[0]).toEqual(new Uint8Array([MAGIC_STATUS_REQ]));
        t.feedReply(
            packStatusRsp({
                state: MachineState.RUNNING,
                axesEnabled: 0x0f,
                axesHomed: 0x0f,
                alarm: AlarmReason.NONE,
                running: RunningReason.JOB,
                bufCount: 3,
                pos: [10, 20, 0, 0],
                expectedSeq: 5,
                queuedUs: 2000,
            }),
        );
        const st = await p;
        expect(st.state).toBe(MachineState.RUNNING);
        expect(st.bufCount).toBe(3);
        expect(st.pos).toEqual([10, 20, 0, 0]);
        expect(st.expectedSeq).toBe(5);
        expect(st.queuedUs).toBe(2000);
    });

    it("throws on timeout (no STATUS_RSP within timeout)", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        await expect(link.getStatus(30)).rejects.toThrow(/no STATUS_RSP/);
    });
});

describe("wire/link/link: status getter", () => {
    it("returns null before any STATUS_RSP arrives", () => {
        const t = new FakeTransport();
        const link = new Link(t);
        expect(link.status).toBeNull();
    });

    it("returns the latest parsed sample after one arrives", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        t.feedReply(
            packStatusRsp({
                state: MachineState.IDLE,
                axesEnabled: 0,
                axesHomed: 0x0f,
                alarm: 0,
                running: 0,
            }),
        );
        // let read loop pump + demux route to the status sink
        await tick(15);
        const st = link.status;
        expect(st?.state).toBe(MachineState.IDLE);
        expect(st?.axesHomed).toBe(0x0f);
    });
});

describe("wire/link/link: resetSeq / abort / send", () => {
    it("resetSeq writes SEQRESET and drains the ACK(0) reply", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.resetSeq();
        await tick();
        expect(t.writes[0]).toEqual(new Uint8Array([MAGIC_SEQRESET]));
        t.feedReply(new Uint8Array([MAGIC_ACK, 0, 0]));
        expect(await p).toBe(true);
    });

    it("resetSeq returns false on timeout", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        expect(await link.resetSeq(30)).toBe(false);
    });

    it("abort() writes the ABORT byte (fire-and-forget)", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        link.abort();
        await tick();
        expect(t.writes[0]).toEqual(new Uint8Array([MAGIC_ABORT]));
    });

    it("send() writes a text line without awaiting a reply", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        await link.send("stop");
        expect(writtenStrings(t)).toEqual(["stop\n"]);
    });
});

describe("wire/link/link: close", () => {
    it("close() closes the transport and the read loop exits", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        await link.close();
        expect(link.closed).toBe(true);
    });
});