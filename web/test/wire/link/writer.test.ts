/**
 * Tests for wire/link/writer — frame-atomic serialisation (D6), batched
 * writes, and the D13 abort-before-commit surface.
 */

import { describe, it, expect } from "vitest";
import { Writer } from "../../../src/wire/link/writer.js";
import { AbortFlag, type Writable } from "../../../src/wire/link/transport.js";

class RecordingWritable implements Writable {
    writes: Uint8Array[] = [];
    write(bytes: Uint8Array): Promise<void> {
        this.writes.push(bytes);
        return Promise.resolve();
    }
}

/** Records start/end of each write so a serialization test can see the order. */
class OrderedWritable implements Writable {
    order: string[] = [];
    write(bytes: Uint8Array): Promise<void> {
        const tag = bytes[0]!.toString();
        this.order.push(`start ${tag}`);
        return new Promise<void>((r) =>
            setTimeout(() => {
                this.order.push(`end ${tag}`);
                r();
            }, 5),
        );
    }
}

function frame(...bytes: number[]): Uint8Array {
    return new Uint8Array(bytes);
}

describe("wire/link/writer: writeFrame", () => {
    it("writes the bytes as one frame", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        await writer.writeFrame(frame(1, 2, 3));
        expect(w.writes.length).toBe(1);
        expect([...w.writes[0]!]).toEqual([1, 2, 3]);
    });

    it("serializes — frame 2 does not start until frame 1's write resolves", async () => {
        const w = new OrderedWritable();
        const writer = new Writer(w);
        await Promise.all([writer.writeFrame(frame(1)), writer.writeFrame(frame(2))]);
        expect(w.order).toEqual(["start 1", "end 1", "start 2", "end 2"]);
    });

    it("writes each frame exactly once (no double-send through the lock chain)", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        await writer.writeFrame(frame(0xaa));
        await writer.writeFrame(frame(0xbb));
        expect(w.writes.length).toBe(2);
    });
});

describe("wire/link/writer: writeText", () => {
    it("appends \\n if missing", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        await writer.writeText("ping");
        expect(new TextDecoder().decode(w.writes[0]!)).toBe("ping\n");
    });

    it("does not double the \\n if already present", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        await writer.writeText("ping\n");
        expect(new TextDecoder().decode(w.writes[0]!)).toBe("ping\n");
    });
});

describe("wire/link/writer: writeBatch", () => {
    it("concatenates the frames into ONE atomic write", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        const written = await writer.writeBatch([frame(1, 2), frame(3, 4), frame(5)]);
        expect(written).toBe(3);
        expect(w.writes.length).toBe(1);
        expect([...w.writes[0]!]).toEqual([1, 2, 3, 4, 5]);
    });

    it("caps at maxBatchFrames", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w, 2);
        const written = await writer.writeBatch([frame(1), frame(2), frame(3), frame(4)]);
        expect(written).toBe(2);
        expect([...w.writes[0]!]).toEqual([1, 2]);
    });

    it("returns 0 for an empty batch", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        expect(await writer.writeBatch([])).toBe(0);
        expect(w.writes.length).toBe(0);
    });

    it("aborts before the commit: returns 0, writes nothing", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        const abort = new AbortFlag();
        abort.set();
        const written = await writer.writeBatch([frame(1), frame(2)], abort);
        expect(written).toBe(0);
        expect(w.writes.length).toBe(0);
        expect(writer.stats().aborts).toBe(1);
    });

    it("a clear abort lets the batch through", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        const abort = new AbortFlag();
        const written = await writer.writeBatch([frame(1), frame(2)], abort);
        expect(written).toBe(2);
        expect(w.writes.length).toBe(1);
    });
});

describe("wire/link/writer: stats", () => {
    it("accumulates frames, batches, bytes", async () => {
        const w = new RecordingWritable();
        const writer = new Writer(w);
        await writer.writeFrame(frame(1, 2));
        await writer.writeBatch([frame(3), frame(4, 5)]);
        const s = writer.stats();
        expect(s.frames).toBe(3);
        expect(s.batches).toBe(1);
        expect(s.bytes).toBe(2 + 1 + 2);
    });
});