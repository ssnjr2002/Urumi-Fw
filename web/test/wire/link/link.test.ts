/**
 * Tests for wire/link/link — the Link that owns a transport, demux, writer and
 * sink set. Covers the control-plane surface (commands one-outstanding, the
 * lines owed to stop and reset, freshStatus, resetSeq, abort, status getter) plus the
 * textDesyncs diagnostic, over a FakeTransport that the test feeds replies
 * into. The real backends (Sim, WebSerial, Node) live in their own commits;
 * this isolates the Link's own behaviour from any specific pipe.
 */

import { describe, it, expect } from "vitest";
import { ConfigTransferError, Link } from "../../../src/link/link.js";
import {
    CFG_MAX_BYTES,
    CFG_NACK_BAD_STATE,
    CFG_NACK_SCHEMA,
    packCfgDataHeader,
    packCfgSetHeader,
} from "../../../src/packets/cfg.js";
import { crc32 } from "../../../src/packets/crc.js";
import type { Transport } from "../../../src/transport/index.js";
import {
    packStatusRsp,
    MachineState,
    JobReason,
} from "../../../src/packets/status.js";
import {
    MAGIC_ACK,
    MAGIC_ABORT,
    MAGIC_CFG_ACK,
    MAGIC_CFG_GET,
    MAGIC_CFG_NACK,
    MAGIC_CFG_RDY,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
    MAGIC_CJOG,
    MAGIC_CJOG_STOP,
    MAGIC_JOG_ACK,
    MAGIC_JOG_NACK,
    NACK_BUSY,
    CJOG_DEADMAN_MS,
} from "../../../src/packets/constants.js";

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
        const p = link.commands().send("ping");
        await tick();
        expect(writtenStrings(t)).toEqual(["ping\n"]);
        t.feedReply(enc.encode("pong\n"));
        expect(await p).toBe("pong");
    });

    it("strips trailing \\r from the reply", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.commands().send("ping");
        await tick();
        t.feedReply(enc.encode("pong\r\n"));
        expect(await p).toBe("pong");
    });

    it("returns '' on timeout", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        expect(await link.commands().send("ping", 30)).toBe("");
    });

    it("serializes concurrent commands (one-outstanding)", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p1 = link.commands().send("ping");
        const p2 = link.commands().send("get pos");
        await tick();
        // p1 holds the text lock awaiting "pong"; p2 has NOT written yet.
        expect(writtenStrings(t).filter((s) => s === "ping\n").length).toBe(1);
        expect(writtenStrings(t).some((s) => s.startsWith("get"))).toBe(false);

        t.feedReply(enc.encode("pong\n"));
        expect(await p1).toBe("pong");

        // now p2 acquires the lock and writes
        await tick();
        expect(writtenStrings(t).filter((s) => s === "get pos\n").length).toBe(1);
        t.feedReply(enc.encode("pos=0,0,0,0\n"));
        expect(await p2).toBe("pos=0,0,0,0");
    });

    it("textDesyncs counts orphan lines drained before a command", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        // An orphan text line lands with no awaiter — a previous command's
        // multi-line reply tail, the contract breach D11 makes loud.
        t.feedReply(enc.encode("stale\n"));
        await tick(10);
        expect(link.stats().textDesyncs).toBe(0); // not counted yet — nothing drained

        const p = link.commands().send("ping");
        await tick();
        t.feedReply(enc.encode("pong\n"));
        expect(await p).toBe("pong");
        expect(link.stats().textDesyncs).toBe(1); // the orphan was drained by command()
    });
});

describe("wire/link/link: freshStatus (binary status plane)", () => {
    it("resolves to the next sample, parsed", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.freshStatus();
        await tick();
        t.feedReply(
            packStatusRsp({
                state: MachineState.JOB,
                faults: 0,
                reason: JobReason.RUNNING,
                bufCount: 3,
                wpos: [10, 20, 0, 0],
                layer: 0,
                record: 5,
            }),
        );
        const st = await p;
        expect(st.state).toBe(MachineState.JOB);
        expect(st.reason).toBe(JobReason.RUNNING);
        expect(st.bufCount).toBe(3);
        expect(st.wpos).toEqual([10, 20, 0, 0]);
        expect(st.layer).toBe(0);
        expect(st.record).toBe(5);
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
                faults: 0,
                reason: 0,
                nodeHomed: 0x00f,
            }),
        );
        // let read loop pump + demux route to the status sink
        await tick(15);
        const st = link.status;
        expect(st?.state).toBe(MachineState.IDLE);
        expect(st?.nodeHomed).toBe(0x00f);
    });
});

describe("wire/link/link: resetSeq / abort", () => {
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

});

describe("wire/link/link: lines owed to stop and reset", () => {
    it("estop() writes stop, and its ok is dropped rather than counted", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        link.estop();
        await tick();
        expect(writtenStrings(t)).toEqual(["stop\n"]);
        t.feedReply(enc.encode("ok\n"));
        await tick(10);
        const p = link.commands().send("ping");
        await tick();
        t.feedReply(enc.encode("pong\n"));
        expect(await p).toBe("pong");
        expect(link.stats().textDesyncs).toBe(0);
    });

    it("a command sent after an estop skips the estop's ok", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        link.estop();
        const p = link.commands().send("ping");
        await tick();
        t.feedReply(enc.encode("ok\npong\n"));
        expect(await p).toBe("pong");
        expect(link.stats().textDesyncs).toBe(0);
    });

    it("an estop sent while a command waits leaves that command its own reply", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.commands().send("ping");
        await tick();
        link.estop();
        t.feedReply(enc.encode("pong\nok\n"));
        expect(await p).toBe("pong");
        const q = link.commands().send("ping");
        await tick();
        t.feedReply(enc.encode("pong\n"));
        expect(await q).toBe("pong");
        expect(link.stats().textDesyncs).toBe(0);
    });

    it("reset reads its ok and drops the boot banner through ready", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.commands().send("reset");
        await tick();
        t.feedReply(enc.encode("ok\nRS485 controller (115200 baud)\nready\n"));
        expect(await p).toBe("ok");
        const q = link.commands().send("ping");
        await tick();
        t.feedReply(enc.encode("pong\n"));
        expect(await q).toBe("pong");
        expect(link.stats().textDesyncs).toBe(0);
    });
});

describe("wire/link/link: config push/pull", () => {
    const blob = new Uint8Array([0x81, 0xa1, 0x76, 0x01]); // {v: 1}

    it("pushConfig sends the header, then the payload only after CFG_RDY", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link._pushEncoded(blob);
        await tick();
        expect(t.writes).toEqual([packCfgSetHeader(blob.length, crc32(blob))]);

        t.feedReply(new Uint8Array([MAGIC_CFG_RDY]));
        await tick();
        expect(t.writes[1]).toEqual(blob);

        t.feedReply(new Uint8Array([MAGIC_CFG_ACK]));
        await expect(p).resolves.toEqual({ bytes: blob.length, crc32: crc32(blob) });
    });

    it("pushConfig surfaces a NACK reason and sends no payload", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link._pushEncoded(blob);
        await tick();
        t.feedReply(new Uint8Array([MAGIC_CFG_NACK, CFG_NACK_BAD_STATE]));
        await expect(p).rejects.toMatchObject({ reason: CFG_NACK_BAD_STATE });
        expect(t.writes.length).toBe(1);
    });

    it("pushConfig surfaces a schema rejection at commit", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link._pushEncoded(blob);
        await tick();
        t.feedReply(new Uint8Array([MAGIC_CFG_RDY]));
        await tick();
        t.feedReply(new Uint8Array([MAGIC_CFG_NACK, CFG_NACK_SCHEMA]));
        await expect(p).rejects.toBeInstanceOf(ConfigTransferError);
    });

    it("pushConfig refuses an oversize blob without writing", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        await expect(link._pushEncoded(new Uint8Array(CFG_MAX_BYTES + 1))).rejects.toThrow(
            /bytes/,
        );
        expect(t.writes.length).toBe(0);
    });

    it("pullConfig returns the payload after checking its CRC", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link._pullEncoded();
        await tick();
        expect(t.writes).toEqual([new Uint8Array([MAGIC_CFG_GET])]);
        t.feedReply(packCfgDataHeader(blob.length, crc32(blob)));
        t.feedReply(blob);
        expect(await p).toEqual(blob);
    });

    it("pullConfig returns null when nothing is stored", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link._pullEncoded();
        await tick();
        t.feedReply(packCfgDataHeader(0, 0));
        expect(await p).toBeNull();
    });

    it("pullConfig rejects a payload that fails its CRC", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link._pullEncoded();
        await tick();
        t.feedReply(packCfgDataHeader(blob.length, crc32(blob) ^ 1));
        t.feedReply(blob);
        await expect(p).rejects.toThrow(/CRC32/);
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
const idle = () => packStatusRsp({ state: MachineState.IDLE, faults: 0, reason: 0 });
const isStatusReq = (w: Uint8Array) => w.length === 1 && w[0] === MAGIC_STATUS_REQ;

describe("wire/link/link: status poll", () => {
    it("refuses a statusPollMs outside 10..1000", () => {
        const t = new FakeTransport();
        expect(() => new Link(t, { statusPollMs: 5 })).toThrow(RangeError);
        expect(() => new Link(t, { statusPollMs: 1500 })).toThrow(RangeError);
    });

    it("asks for the status every statusPollMs until the port closes", async () => {
        const t = new FakeTransport();
        const link = new Link(t, { statusPollMs: 10 });
        await tick(60);
        expect(t.writes.filter(isStatusReq).length).toBeGreaterThanOrEqual(3);
        await link.close();
        await tick(20);
        const n = t.writes.length;
        await tick(40);
        expect(t.writes.length).toBe(n);
    });

    it("calls every onStatus listener; off() stops only its own", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const a: number[] = [];
        const b: number[] = [];
        const offA = link.onStatus((s) => a.push(s.state));
        link.onStatus((s) => b.push(s.state));
        t.feedReply(idle());
        await tick(15);
        offA();
        t.feedReply(idle());
        await tick(15);
        expect(a.length).toBe(1);
        expect(b.length).toBe(2);
        await link.close();
    });

    it("freshStatus resolves on the next sample and rejects when the port closes", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const p = link.freshStatus();
        t.feedReply(idle());
        expect((await p).state).toBe(MachineState.IDLE);
        const q = link.freshStatus();
        await link.close();
        await expect(q).rejects.toThrow(/closed/);
    });
});

describe("wire/link/link: cjog", () => {
    const jogs = (t: FakeTransport) => t.writes.filter((w) => w[0] === MAGIC_CJOG);

    it("refuses more than one axis set", () => {
        const link = new Link(new FakeTransport());
        expect(() => link.cjog({ x: 1, z: 1 })).toThrow(RangeError);
        expect(() => link.cjog({})).toThrow(RangeError);
    });

    it("repeats the packet while running, and stop() sends CJOG_STOP and stops repeating", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const jog = link.cjog({ x: 1 });
        t.feedReply(new Uint8Array([MAGIC_JOG_ACK]));
        expect(await jog.started).toEqual({ ok: true });
        await tick(CJOG_DEADMAN_MS + 20);
        expect(jogs(t).length).toBeGreaterThanOrEqual(3);
        jog.stop();
        expect(jog.running).toBe(false);
        await tick();
        const n = jogs(t).length;
        expect(t.writes.some((w) => w.length === 1 && w[0] === MAGIC_CJOG_STOP)).toBe(true);
        await tick(CJOG_DEADMAN_MS);
        expect(jogs(t).length).toBe(n);
        await link.close();
    });

    it("a refusal stops the jog and carries the reason", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const jog = link.cjog({ z: -1 });
        t.feedReply(new Uint8Array([MAGIC_JOG_NACK, NACK_BUSY]));
        expect(await jog.started).toEqual({ ok: false, reason: NACK_BUSY });
        expect(jog.running).toBe(false);
        expect(() => jog.turn({ z: 1 })).toThrow(/not running/);
        await link.close();
    });

    it("turn() answers with the new start's reply; the same direction needs none", async () => {
        const t = new FakeTransport();
        const link = new Link(t);
        const jog = link.cjog({ x: 1 });
        t.feedReply(new Uint8Array([MAGIC_JOG_ACK]));
        await jog.started;
        expect(await jog.turn({ x: 1 })).toEqual({ ok: true });
        const p = jog.turn({ x: -1 });
        t.feedReply(new Uint8Array([MAGIC_JOG_ACK]));
        expect(await p).toEqual({ ok: true });
        jog.stop();
        await link.close();
    });
});
