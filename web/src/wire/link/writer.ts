/**
 * link/writer.ts — frame-granular exclusive writer.
 * Ported from host/protocol/writer.py.
 *
 * Demultiplexing is hard; multiplexing is easy. The writer knows what it is
 * sending and needs none of the reader's machinery. It has exactly one job:
 * ATOMICITY — never let two concerns interleave bytes within a frame (D6).
 *
 * The hazard is concrete: if a status poll writes 0xA5 midway through a 26-byte
 * MSEG, the firmware is in RX_FIXED26 and takes it UNCONDITIONALLY as packet
 * payload. The MSEG then fails CRC and the status request is silently eaten.
 * So ownership granularity mirrors the reader's: the reader owns the port
 * always, the writer owns it PER FRAME, released between. That is what lets a
 * poll slot between two MSEGs without seizing anything.
 *
 * Batching is compatible with atomicity (D6/D7): the atomic unit is not one
 * packet but a WHOLE NUMBER of frames. A 16-packet window write is one
 * acquisition, one write — 416 bytes, still atomic, and per the transport
 * substrate (docs/comms_architecture.md §1: cost is per-transaction, not
 * per-byte) that is where the throughput win lives. Batches stay bounded so a
 * writer holding the lock for a session does not reintroduce seizure.
 *
 * The writer does not read (D8). A caller writing a text command awaits the
 * text sink for its reply rather than reading inline; that decoupling is what
 * allows a poll to be in flight while a stream is writing.
 *
 * `writeBatch(frames, abort)` is the D13 surface: `abort` is checked before
 * the batch commits, so an urgent truncate waits at most one frame, never a
 * whole batch. Returning the count (rather than throwing) lets a caller
 * rewind its window precisely — unwritten frames were never sent, so they
 * need no retransmission logic, just a smaller send cursor.
 */

import type { AbortToken, Writable } from "./transport.js";

export interface WriterStats {
    readonly frames: number;
    readonly batches: number;
    readonly bytes: number;
    readonly aborts: number;
    readonly avgBatch: number;
}

export class Writer {
    private _writable: Writable;
    private _maxBatch: number;
    private _lock: Promise<void> = Promise.resolve();

    private _frames = 0;
    private _batches = 0;
    private _bytes = 0;
    private _aborts = 0;

    constructor(writable: Writable, maxBatchFrames = 16) {
        this._writable = writable;
        this._maxBatch = maxBatchFrames;
    }

    /** Write one frame atomically. Used for status polls, text lines, one-off binary ops. */
    writeFrame(data: Uint8Array): Promise<void> {
        return this._withLock(async () => {
            await this._writable.write(data);
            this._frames++;
            this._bytes += data.length;
        });
    }

    /** Write a control-plane text line ( appending `\n` if missing). Does NOT read the reply. */
    writeText(line: string): Promise<void> {
        const withNewline = line.endsWith("\n") ? line : line + "\n";
        return this.writeFrame(new TextEncoder().encode(withNewline));
    }

    /**
     * Write up to `maxBatchFrames` frames as one transfer, and return how many
     * were actually written.
     *
     * `abort` is checked BEFORE the write (D13): truncation happens before the
     * commit, so the batch that goes out is the batch that lands whole, and an
     * abort costs at most the in-flight write — never a partial frame on the
     * normal path. Returning the count lets a session rewind its window
     * precisely: unwritten frames were never sent, so they need no retransmit,
     * just a smaller `next`.
     */
    writeBatch(frames: readonly Uint8Array[], abort?: AbortToken): Promise<number> {
        if (frames.length === 0) return Promise.resolve(0);
        return this._withLock(async () => {
            const batch = frames.slice(0, this._maxBatch);
            if (abort !== undefined && abort.isSet()) {
                this._aborts++;
                return 0;
            }
            const total = batch.reduce((n, f) => n + f.length, 0);
            const buf = new Uint8Array(total);
            let off = 0;
            for (const f of batch) {
                buf.set(f, off);
                off += f.length;
            }
            await this._writable.write(buf);
            this._frames += batch.length;
            this._batches++;
            this._bytes += total;
            return batch.length;
        });
    }

    stats(): WriterStats {
        return {
            frames: this._frames,
            batches: this._batches,
            bytes: this._bytes,
            aborts: this._aborts,
            avgBatch: this._batches ? Math.round((this._frames / this._batches) * 10) / 10 : 0,
        };
    }

    /**
     * Acquire the per-frame lock: await the previous holder's release, then run
     * `fn` ONCE while holding. The lock is a promise chain — each call's `prev`
     * is the previous holder's completion, so writes serialise without
     * blocking the event loop. `fn` runs exactly once: the returned promise
     * resolves with its result, and `this._lock` advances to its completion so
     * the next caller waits. This is the existing _serialize() mechanism, held
     * per FRAME rather than per request-plus-read.
     */
    private _withLock<T>(fn: () => Promise<T>): Promise<T> {
        const prev = this._lock;
        const result = prev.then(() => fn());
        // The next caller awaits `result`'s completion before running, errors
        // swallowed so a failed write does not permanently wedge the chain.
        this._lock = result.then(
            () => undefined,
            () => undefined,
        );
        return result;
    }
}