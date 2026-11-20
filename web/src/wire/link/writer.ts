/**
 * link/writer.ts — frame-granular exclusive writer (docs/system/wire.md).
 *
 * Its one job: never let two callers interleave bytes within a frame. A
 * STATUS_REQ byte written inside a record would be taken by the controller as
 * part of the record, which then fails its CRC. The lock is held per write,
 * released between, so a poll can slip in between two records.
 *
 * A write may be a whole number of frames: a window of up to 16 records goes
 * out as one transfer, since USB costs per transfer, not per byte. Batches stay
 * bounded so a stream never holds the lock for long.
 *
 * The writer does not read; a caller awaits its reply on a sink.
 *
 * `writeBatch(frames, abort)` checks `abort` before writing and returns how many
 * frames it wrote, so a caller rewinds its window precisely.
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
     * `abort` is checked BEFORE the write: truncation happens before the
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