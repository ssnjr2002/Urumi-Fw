/**
 * link/transport.ts — the environment-agnostic I/O contract a backend implements.
 *
 * The link layer (sink, demux, writer, session, link) speaks ONLY this
 * interface; a backend — the in-process Sim, a browser WebSerial port, or a
 * Node serialport — adapts its environment to it. That is what makes the
 * comms layer usable from a browser UI OR a Node CLI: everything above `Link`
 * is environment-blind, and the three backends are thin.
 *
 * `write` returns once the bytes are accepted by the underlying stream
 * (there is no separate `flush` — the browser/Node serial impl owns buffering,
 * and the Writer's atomicity guarantee is about not interleaving frames within
 * a single `write`, which the underlying stream already preserves for one
 * chunk). `read` is an async iterable of byte chunks; frame boundaries need
 * not align with chunk boundaries, which is the normal case at the 64-byte
 * USB packet quantum — the Demux consumes them blind.
 *
 * `AbortToken` is the D13 hook: the Writer checks it before committing a
 * batch (bounded to never hold the write lock for a whole session), and a
 * Session's `truncate()` sets it so an urgent abort waits at most one frame
 * rather than a whole batch.
 */

/** Bytes-out surface — the Writer needs only this. */
export interface Writable {
    write(bytes: Uint8Array): Promise<void>;
}

/** Bytes-in surface — a backend pumps chunks into the Demux via this. */
export interface Readable {
    read(): AsyncIterable<Uint8Array>;
}

/** A backend: bidirectional byte pipe that the Link owns for a connection. */
export interface Transport extends Writable, Readable {
    close(): Promise<void>;
}

/** Flag the Writer polls before committing a batch (D13). */
export interface AbortToken {
    isSet(): boolean;
}

/**
 * Default AbortToken: a boolean flag. A Session constructs one and `truncate()`
 * sets it; the Writer's `writeBatch` sees it and returns 0 without writing,
 * so the in-flight batch never goes out and the caller rewinds to `base`.
 */
export class AbortFlag implements AbortToken {
    private _set = false;
    set(): void {
        this._set = true;
    }
    clear(): void {
        this._set = false;
    }
    isSet(): boolean {
        return this._set;
    }
}