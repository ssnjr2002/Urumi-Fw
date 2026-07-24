/**
 * link/sink.ts — typed awaitable queues + latest-wins slots.
 * Ported from host/protocol/reader.py (Sink, LatestSink, make_sinks).
 *
 * The Demux fans every inbound frame out to a typed sink, one per frame
 * class. Routing on magic is what removes the need to flush the port before
 * a request (D10): a stale status reply lands in the status sink, never in
 * the text sink, so it cannot be mistaken for a command's reply.
 *
 * Two kinds, deliberately different:
 *
 *   Sink<T>        — ordered, FIFO, never drops. Acks and text carry
 *                    information the consumer needs on every frame, so a
 *                    backlog is real and must not be lost. Awaitable via
 *                    get(timeout) — resolves on the next put, or null on
 *                    timeout.
 *
 *   LatestSink<T>  — latest-wins, no accumulation. Status is a state
 *                    sample, not a transaction result: a slightly-late reply
 *                    is still a genuine, slightly-older sample that the next
 *                    poll corrects (D9). So an old one is worthless rather
 *                    than merely late, and the sink overwrites instead of
 *                    queuing. `waitUpdate(since, timeout)` wakes on a sample
 *                    newer than `since` — pass the previously-returned stamp
 *                    to avoid missing an update that landed between calls.
 *
 * The browser's single-threaded async model is what sets the shape (D12):
 * sinks are awaitable promises, not blocking queues. Python mirrors the
 * interface with `queue.Queue` + a `threading.Condition`; the API is designed
 * for the async model, threads implement it, not the reverse.
 */

/** Sink<T> — ordered, never drops, awaitable. The ack and text sinks. */
export class Sink<T> {
    readonly name: string;
    private _q: T[] = [];
    private _waiters: Array<{
        resolve: (v: T | null) => void;
        timer: ReturnType<typeof setTimeout> | null;
    }> = [];

    constructor(name: string) {
        this.name = name;
    }

    /** Enqueue an item, or hand it straight to a waiting consumer. */
    put(item: T): void {
        const w = this._waiters.shift();
        if (w) {
            if (w.timer) clearTimeout(w.timer);
            w.resolve(item);
            return;
        }
        this._q.push(item);
    }

    /**
     * Resolve on the next item, or `null` on timeout. A timeout of
     * `undefined` (or <= 0) waits indefinitely; a finite timeout is the
     * Session's ACK-wait and the Link's command-reply wait.
     */
    get(timeoutMs?: number): Promise<T | null> {
        if (this._q.length > 0) return Promise.resolve(this._q.shift()!) as Promise<T | null>;
        return new Promise<T | null>((resolve) => {
            const waiter = { resolve, timer: null as ReturnType<typeof setTimeout> | null };
            this._waiters.push(waiter);
            if (timeoutMs !== undefined && timeoutMs > 0) {
                waiter.timer = setTimeout(() => {
                    const i = this._waiters.indexOf(waiter);
                    if (i >= 0) this._waiters.splice(i, 1);
                    resolve(null);
                }, timeoutMs);
            }
        });
    }

    /**
     * Drop everything queued; return how many were dropped. Used by the text
     * plane's one-outstanding rule (D11) — `command()` clears orphans before
     * issuing so a previous command's multi-line tail cannot desync the next.
     * Does not touch outstanding get() waiters (their awaiter is the current
     * command, which has not yet been issued).
     */
    clear(): number {
        const n = this._q.length;
        this._q = [];
        return n;
    }

    get length(): number {
        return this._q.length;
    }
}

/**
 * LatestSink<T> — latest-wins slot. The status sink. `value` is a field
 * read, not a round trip; `waitUpdate(since, timeout)` blocks until a sample
 * newer than `since` arrives (or the timeout fires), returning [value, stamp]
 * or [null, stamp]. `sample` is the consistent (value, stamp, arrival_time)
 * triple a source extrapolating from a sample needs — it cannot tell a fresh
 * sample from a repeated value reading `.value` twice, and a sample without
 * an arrival time cannot be aged.
 */
export class LatestSink<T> {
    readonly name: string;
    private _value: T | null = null;
    private _stamp = 0;
    private _at = 0;
    private _waiters: Array<{
        resolve: (v: readonly [T, number] | readonly [null, number]) => void;
        timer: ReturnType<typeof setTimeout> | null;
        start: number;
    }> = [];

    constructor(name: string) {
        this.name = name;
    }

    /** Publish a fresh sample; wake every waiter waiting for a newer one. */
    put(item: T): void {
        this._value = item;
        this._stamp++;
        this._at = nowMonotonic();

        const ws = this._waiters;
        this._waiters = [];
        for (const w of ws) {
            if (this._stamp > w.start) {
                if (w.timer) clearTimeout(w.timer);
                w.resolve([item, this._stamp] as const);
            } else {
                this._waiters.push(w); // not newer than what it asked for
            }
        }
    }

    /** Most recent sample, or null if none has arrived. Non-blocking. */
    get value(): T | null {
        return this._value;
    }

    /** (value, stamp, arrival_time) — a consistent triple for extrapolation. */
    get sample(): readonly [T | null, number, number] {
        return [this._value, this._stamp, this._at];
    }

    /**
     * Block until a sample newer than `since` arrives (return immediately if
     * one already has), or until `timeoutMs`. Returns [value, stamp]; value is
     * null only if the timeout fired before any sample ever arrived. Pass the
     * previously-returned stamp as `since` to avoid missing an update that
     * landed between calls.
     */
    waitUpdate(
        timeoutMs?: number,
        since?: number,
    ): Promise<readonly [T, number] | readonly [null, number]> {
        if (since !== undefined && this._stamp > since && this._value !== null) {
            return Promise.resolve([this._value, this._stamp] as const);
        }
        const start = this._stamp;
        return new Promise<readonly [T, number] | readonly [null, number]>((resolve) => {
            const waiter = {
                resolve,
                timer: null as ReturnType<typeof setTimeout> | null,
                start,
            };
            this._waiters.push(waiter);
            if (timeoutMs !== undefined && timeoutMs > 0) {
                waiter.timer = setTimeout(() => {
                    const i = this._waiters.indexOf(waiter);
                    if (i >= 0) this._waiters.splice(i, 1);
                    resolve([null, this._stamp] as const);
                }, timeoutMs);
            }
        });
    }
}

function nowMonotonic(): number {
    return typeof performance !== "undefined" ? performance.now() : Date.now();
}