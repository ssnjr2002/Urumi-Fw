/**
 * Tests for wire/link/demux — the pure demultiplexing state machine (D1–D5).
 *
 * The key pin is the phantom-ACK hazard: a CFG_DATA payload is an opaque
 * msgpack blob that can contain 0xAA, 0xBB, 0xA6 — bytes that a magic
 * scanner would mis-read as framing. This port mirrors the Python guarantee
 * (host/protocol/reader.py, pinned by host/diagnostics/test_reader.py
 * `test_cfg_data_payload_full_of_magics`): the demux consumes the payload
 * OPAQUELY by count, never scanning it, so no ack-sink put happens from
 * inside config data.
 */

import { describe, it, expect } from "vitest";
import { Demux, makeSinks, Ack, Nack, CfgReply } from "../../../src/wire/link/demux.js";
import { packStatusRsp } from "../../../src/wire/format/status.js";
import { packCfgDataHeader } from "../../../src/wire/format/cfg.js";
import {
    MAGIC_ACK,
    MAGIC_NACK,
    MAGIC_CFG_RDY,
    MAGIC_CFG_ACK,
    MAGIC_CFG_NACK,
    MAGIC_CFG_DATA,
} from "../../../src/wire/format/constants.js";

function concat(...parts: Uint8Array[]): Uint8Array {
    const total = parts.reduce((n, p) => n + p.length, 0);
    const out = new Uint8Array(total);
    let off = 0;
    for (const p of parts) {
        out.set(p, off);
        off += p.length;
    }
    return out;
}

describe("wire/link/demux: ACK / NACK routing", () => {
    it("routes an ACK to the ack sink as Ack(expectedSeq)", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new Uint8Array([MAGIC_ACK, 7, 0]));
        const r = await sinks.ack.get(50);
        expect(r).toBeInstanceOf(Ack);
        expect((r as Ack).expectedSeq).toBe(7);
        expect(d.idle).toBe(true);
    });

    it("routes a NACK to the ack sink as Nack(reason)", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new Uint8Array([MAGIC_NACK, 0x02, 0]));
        const r = await sinks.ack.get(50);
        expect(r).toBeInstanceOf(Nack);
        expect((r as Nack).reason).toBe(0x02);
    });

    it("delivers frames in order from one feed", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(concat(new Uint8Array([MAGIC_ACK, 1, 0]), new Uint8Array([MAGIC_ACK, 2, 0])));
        expect(await sinks.ack.get(50)).toEqual(new Ack(1));
        expect(await sinks.ack.get(50)).toEqual(new Ack(2));
    });
});

describe("wire/link/demux: status routing", () => {
    it("hands a 30-byte STATUS_RSP to the status sink intact", () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        const ref = packStatusRsp({
            state: 1,
            axesEnabled: 0x0f,
            axesHomed: 0x06,
            alarm: 0,
            running: 0,
            bufCount: 3,
            pos: [10, -5, 0, 0],
            expectedSeq: 9,
            queuedUs: 1000,
        });
        d.feed(ref);
        // status is latest-wins: a field read, not a round trip (D9)
        const v = sinks.status.value;
        expect(v).not.toBeNull();
        expect([...v!]).toEqual([...ref]);
    });

    it("consumes the frame even when split across chunks (byte-by-byte)", () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        const ref = packStatusRsp({ state: 0, axesEnabled: 0, axesHomed: 0, alarm: 0, running: 0 });
        for (let i = 0; i < ref.length; i++) {
            d.feed(ref.subarray(i, i + 1));
        }
        const v = sinks.status.value;
        expect(v).not.toBeNull();
        expect([...v!]).toEqual([...ref]);
        expect(d.idle).toBe(true);
    });
});

describe("wire/link/demux: text routing", () => {
    it("delivers a \\n-terminated line to the text sink, trailing \\r stripped", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new TextEncoder().encode("pong\r\n"));
        expect(await sinks.text.get(50)).toBe("pong");
    });

    it("ignores blank lines (bare \\n does not produce a text put)", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new TextEncoder().encode("\n\npong\n"));
        expect(await sinks.text.get(50)).toBe("pong");
        expect(sinks.text.length).toBe(0);
    });

    it("does not consume a line beyond MAX_TEXT_LINE — resyncs on overrun", () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        // exactly MAX_TEXT_LINE + 1 non-newline bytes: the first 512 fill the
        // buffer, the 513th trips the guard, the demux resets to idle.
        const longLine = new Uint8Array(513); // MAX_TEXT_LINE (512) + 1
        longLine.fill(0x61); // 'a'
        d.feed(longLine);
        expect(d.overruns).toBe(1);
        expect(d.idle).toBe(true);
    });

    it("resyncs after a text overrun — the next frame still routes", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        const longLine = new Uint8Array(513);
        longLine.fill(0x61);
        d.feed(longLine);
        // a valid ACK right after the overrun arrives cleanly
        d.feed(new Uint8Array([MAGIC_ACK, 4, 0]));
        expect((await sinks.ack.get(50) as Ack).expectedSeq).toBe(4);
    });
});

describe("wire/link/demux: config plane", () => {
    it("routes CFG_RDY (1-byte) to the cfg sink", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new Uint8Array([MAGIC_CFG_RDY]));
        const r = await sinks.cfg.get(50);
        expect(r).toBeInstanceOf(CfgReply);
        expect(r!.kind).toBe(MAGIC_CFG_RDY);
    });

    it("routes CFG_ACK (1-byte) to the cfg sink", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new Uint8Array([MAGIC_CFG_ACK]));
        const r = await sinks.cfg.get(50);
        expect(r!.kind).toBe(MAGIC_CFG_ACK);
    });

    it("routes CFG_NACK (2-byte, reason) to the cfg sink", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new Uint8Array([MAGIC_CFG_NACK, 0x03]));
        const r = await sinks.cfg.get(50);
        expect(r!.kind).toBe(MAGIC_CFG_NACK);
        expect(r!.reason).toBe(0x03);
    });

    it("routes a length=0 CFG_DATA to the cfg sink with an empty payload", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(packCfgDataHeader(0, 0xdeadbeef));
        const r = await sinks.cfg.get(50);
        expect(r!.kind).toBe(MAGIC_CFG_DATA);
        expect(r!.payload).toEqual(new Uint8Array(0));
        expect(r!.crc32).toBe(0xdeadbeef);
    });

    it("routes a CFG_DATA header + payload, payload intact", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        const payload = new Uint8Array([1, 2, 3, 4, 5]);
        d.feed(concat(packCfgDataHeader(payload.length, 0xcafe), payload));
        const r = await sinks.cfg.get(50);
        expect(r!.kind).toBe(MAGIC_CFG_DATA);
        expect([...r!.payload!]).toEqual([1, 2, 3, 4, 5]);
        expect(r!.crc32).toBe(0xcafe);
        expect(d.idle).toBe(true);
    });
});

describe("wire/link/demux: phantom-ACK hazard (D2)", () => {
    it("a CFG_DATA payload full of magic bytes never reaches the ack sink", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        // A payload that would emit phantom ACKs/NACKs/STATUSes from inside a
        // scanner: 0xAA (ACK), 0xBB (NACK), 0xA6 (retired status), 0xA9 (ABORT),
        // 0xA8 (SEQRESET), 0xAB (MSEG), 0xAE (JOG), 0xA5 (STATUS_REQ), 0xA7.
        const payload = new Uint8Array([0xaa, 0xbb, 0xa6, 0xa9, 0xa8, 0xab, 0xae, 0xa5, 0xa7]);
        const frame = concat(packCfgDataHeader(payload.length, 0x1234), payload);
        d.feed(frame);

        // The ack sink is empty — the payload was consumed opaquely by count,
        // not scanned. No phantom Ack/Nack from inside config data.
        expect(sinks.ack.length).toBe(0);
        expect(await sinks.ack.get(20)).toBeNull();
        // The status sink is empty too.
        expect(sinks.status.value).toBeNull();

        // The cfg sink got exactly one CFG_DATA with the payload intact.
        const r = await sinks.cfg.get(50);
        expect(r!.kind).toBe(MAGIC_CFG_DATA);
        expect([...r!.payload!]).toEqual([...payload]);
        expect(r!.crc32).toBe(0x1234);

        // And the demux is idle again, ready for the next frame — it did not
        // get stranded mid-payload.
        expect(d.idle).toBe(true);
        expect(d.unknownBytes).toBe(0);
    });

    it("an oversized CFG_DATA length does not blind the demux — resyncs on overrun", () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        // length = MAX_CFG_PAYLOAD + 1 — the demux must NOT enter the payload
        // state and consume that many bytes of whatever follows.
        const oversizedLen = 64 * 1024 + 1;
        d.feed(packCfgDataHeader(oversizedLen, 0));
        expect(d.overruns).toBe(1);
        expect(d.idle).toBe(true);
        // The next byte is dispatched fresh (not swallowed as payload).
        d.feed(new Uint8Array([MAGIC_ACK, 3, 0]));
        // (just asserting no throw / it's classifiable; the ack arrives:)
        return sinks.ack.get(50).then((r) => expect((r as Ack).expectedSeq).toBe(3));
    });
});

describe("wire/link/demux: unknown bytes (D5)", () => {
    it("discards one unknown high byte and stays idle", () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(new Uint8Array([0xcf]));
        expect(d.unknownBytes).toBe(1);
        expect(d.idle).toBe(true);
    });

    it("resyncs: unknown byte then a valid frame", async () => {
        const sinks = makeSinks();
        const d = new Demux(sinks);
        d.feed(concat(new Uint8Array([0xcf]), new Uint8Array([MAGIC_ACK, 5, 0])));
        expect(d.unknownBytes).toBe(1);
        expect((await sinks.ack.get(50) as Ack).expectedSeq).toBe(5);
    });
});