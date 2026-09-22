/**
 * link/session.ts — windowed stream sessions over the demux (D14 / §2.3).
 * Ported from host/protocol/session.py.
 *
 * A Session subscribes to the ack sink for its span instead of seizing the
 * port and spawning its own reader. Status polling and text commands stay live
 * throughout — the architectural change from the old Sender, not the Go-Back-N
 * logic (that is carried over intact).
 *
 * CLOSED vs OPEN sessions fall out of one signature once the retransmit buffer
 * is separated from the packet source:
 *
 *   closed (job)  — pull() returns slices, then null. The whole sequence is
 *                   known before the first byte goes out.
 *   open  (jog)   — pull() may return [] indefinitely (operator not pressing,
 *                   buffer full) and null only when winding down. The total
 *                   is not known up front; it depends on input that has not
 *                   happened yet.
 *
 * The property that makes open sessions tractable: pull() is called AT MOST
 * ONCE PER PACKET, EVER. A go-back replays from the retained window, never
 * from the source — so a source needs no idempotency and no memory of what it
 * already produced, which is exactly what a jog source cannot provide.
 *
 *   pull() → null    : finished (closed: exhausted; open: winding down)
 *   pull() → []      : nothing right now, still open
 *   pull() → [pkts]  : emit these
 */

import type { Sink, LatestSink } from "./sink.js";
import type { Writer } from "./writer.js";
import { NACK_BAD_MAGIC, NACK_PAUSED, NACK_BAD_STATE, NACK_CRC, NACK_FULL, NACK_ABORTING } from "../format/constants.js";
import { stampSeq } from "../format/packet.js";
import { Ack, Nack } from "./demux.js";
import { parseStatusRsp, type MachineStatus } from "../format/status.js";

export const DEFAULT_WINDOW = 16;
const ACK_TIMEOUT_MS = 200; // per-response wait before assuming loss
const STALL_TIMEOUT_MS = 3000; // total silence before a fatal abort
const BACKPRESSURE_MS = 50; // wait for the ring to drain on NACK_FULL
const MAX_CRC_ERRORS = 20;
const IDLE_WAIT_MS = 20; // bound on an open source's idle wait

// ── fatal reason codes (D17) ────────────────────────────────────────────────────
// run() returns false on any fatal NACK OR on a self-inflicted failure (total
// silence past STALL_TIMEOUT_MS, too many CRC retries). Before D17 those paths
// returned a bare `false` and the caller had no way to tell them apart — a
// machine that NACKed BAD_STATE vs one that simply went silent looked the same.
// `fatalReason` is set at each fatal exit and `fatalReasonName()` renders it.
//
// NACK reasons (0x01..0x07) are passed through unchanged; self-inflicted ones
// use the 0xE0.. range so they never collide with a future wire NACK.
export const FATAL_STALL = 0xe0; // no ACK/NACK past STALL_TIMEOUT_MS
export const FATAL_CRC_LIMIT = 0xe1; // NACK_CRC count exceeded MAX_CRC_ERRORS

function delay(ms: number): Promise<void> {
    return new Promise((r) => setTimeout(r, ms));
}

// ── what a source sees ────────────────────────────────────────────────────────

/**
 * Handed to PacketSource.pull(). Everything a source needs to decide what to
 * emit next, without owning the port or issuing a round trip.
 *
 * `status` is the key one: the live sample from the global status sink, which
 * keeps updating DURING transmission because the reader never stopped reading.
 * Under the old seizure model a jog's blend-vs-decel decision was made on a
 * sample stale by the whole duration of the send just completed.
 */
export class StreamContext {
    private readonly _session: Session;
    private readonly _statusSink: LatestSink<Uint8Array> | undefined;

    /** Free slots in the window (window - in_flight). Cleared before pull(). */
    room = 0;
    /** Packets emitted but not yet cumulatively ACKed. */
    inFlight = 0;
    /** Total packets pulled from the source (terminal: run() ends when base >= emitted). */
    emitted = 0;
    /** Total packets cumulatively ACKed so far. */
    acked = 0;

    constructor(session: Session, statusSink: LatestSink<Uint8Array> | undefined) {
        this._session = session;
        this._statusSink = statusSink;
    }

    /** Latest status snapshot, or null if none has arrived. A field read, not a round trip. */
    get status(): MachineStatus | null {
        const raw = this._statusSink?.value;
        if (!raw) return null;
        try {
            return parseStatusRsp(raw);
        } catch {
            return null;
        }
    }

    /** MicroSegments queued on the Pico, or null. A jog source watches this to pace. */
    get bufCount(): number | undefined {
        return this.status?.bufCount;
    }

    /**
     * (queued_us, stamp, arrival_time) — or (undefined, 0, 0). For a source that
     * EXTRAPOLATES between polls: `stamp` says whether this sample was already
     * accounted for, `arrivalTime` says how old it is. Reading `queuedUs`
     * repeatedly cannot tell a fresh sample from a stale one.
     */
    get queuedSample(): readonly [number | undefined, number, number] {
        const sink = this._statusSink;
        if (!sink) return [undefined, 0, 0];
        const [raw, stamp, at] = sink.sample;
        if (!raw) return [undefined, stamp, at];
        try {
            return [parseStatusRsp(raw).queuedUs, stamp, at];
        } catch {
            return [undefined, stamp, at];
        }
    }

    /** Queued MOTION TIME on the Pico in microseconds, or null. The right pacing measure. */
    get queuedUs(): number | undefined {
        return this.status?.queuedUs;
    }

    /**
     * Bounded wait for an open source with nothing to emit. Resolves either
     * after `timeoutMs` or when the session is truncated (so an abort / jog
     * reversal wakes a waiting source immediately).
     *
     * THE PYTHON/TS DIVERGENCE (§2.3): Python blocks on a `threading.Event`
     * that a click can wake. A source with its own intent queue should supply
     * its own wait target — `Promise.race([intentPromise, this._session._abortPromise])`
     * — so jogging does not gain a latency floor equal to a poll interval.
     * This default is the fallback, racing the bare timeout against truncate.
     */
    wait(timeoutMs: number = IDLE_WAIT_MS): Promise<void> {
        return Promise.race([delay(timeoutMs), this._session.abortPromise]);
    }
}

/** Interface. pull() returns: null (finished), [] (nothing right now, still open), [pkts] (emit). */
export interface PacketSource {
    pull(ctx: StreamContext): Promise<Uint8Array[] | null>;
}

/** Closed session: the whole sequence is known up front. */
export class ListSource implements PacketSource {
    private readonly _packets: Uint8Array[];
    private _i = 0;

    constructor(packets: readonly Uint8Array[]) {
        this._packets = [...packets];
    }

    get length(): number {
        return this._packets.length;
    }

    pull(_ctx: StreamContext): Promise<Uint8Array[] | null> {
        if (this._i >= this._packets.length) return Promise.resolve(null);
        const take = Math.max(1, _ctx.room);
        const chunk = this._packets.slice(this._i, this._i + take);
        this._i += chunk.length;
        return Promise.resolve(chunk);
    }
}

// ── the session ───────────────────────────────────────────────────────────────

export interface SessionStats {
    readonly emitted: number;
    readonly sent: number;
    readonly acked: number;
    readonly nacks: number;
    readonly retries: number;
    readonly truncated: boolean;
}

export class Session {
    private readonly _writer: Writer;
    private readonly _ackSink: Sink<Ack | Nack>;
    private readonly _source: PacketSource;
    private readonly _statusSink: LatestSink<Uint8Array> | undefined;
    readonly window: number;
    /** When set, run() emits per-event console.debug lines (D17): ACK seq+delta,
     *  NACK + reason name, go-back to base, stall, fatal. Idle by default so a
     *  normal stream makes no noise; flip on to trace a silent failure. */
    verbose = false;

    private readonly _ctx: StreamContext;
    private readonly _abortResolve: () => void;
    private readonly _abortPromise: Promise<void>;

    // Retransmit buffer: packets emitted but not yet cumulatively ACKed.
    // Separate from the source — this is what makes pull() once-per-packet.
    private _window: Uint8Array[] = [];
    private _base = 0; // count of packets confirmed accepted
    private _next = 0; // count of packets handed to the writer
    private _emitted = 0; // count of packets pulled from the source
    private _seq = 0; // rolling 8-bit wire seq, stamped at emit time
    private _done = false; // source returned null

    sent = 0;
    acked = 0;
    retries = 0;
    nacks = 0;
    truncated = false;
    /** Why run() returned false (D17). Unset on a successful/truncated run.
     *  For a NACK it is the NACK reason byte; FATAL_STALL / FATAL_CRC_LIMIT
     *  are self-inflicted. Read via `fatalReasonName()` for a human label. */
    fatalReason: number | undefined;

    constructor(
        writer: Writer,
        ackSink: Sink<Ack | Nack>,
        source: PacketSource,
        statusSink?: LatestSink<Uint8Array>,
        window: number = DEFAULT_WINDOW,
    ) {
        this._writer = writer;
        this._ackSink = ackSink;
        this._source = source;
        this._statusSink = statusSink;
        this.window = window;
        this._ctx = new StreamContext(this, statusSink);
        // Local-then-assign: a `readonly` field can be assigned in the
        // constructor body, but not inside a nested Promise callback. Capturing
        // the resolver in a local first lets the field stay readonly.
        let resolve!: () => void;
        this._abortPromise = new Promise<void>((r) => {
            resolve = r;
        });
        this._abortResolve = resolve;
    }

    /** The promise that resolves when truncate() is called — sources race against it. */
    get abortPromise(): Promise<void> {
        return this._abortPromise;
    }

    get emitted(): number {
        return this._emitted;
    }

    /**
     * End the session at the next frame boundary. Thread-safe enough for the
     * async model: sets the flag the run loop checks at the top, and resolves
     * the abort promise so a source blocked in ctx.wait() wakes immediately.
     * Whatever the source wanted next is discarded. It does not touch the writer,
     * so it cannot corrupt a frame already in progress. Truncation is a normal
     * outcome — run() returns true.
     */
    truncate(): void {
        this.truncated = true;
        this._abortResolve();
    }

    /**
     * Drive the stream to completion. Returns true on success or truncation,
     * false on fatal — in which case `fatalReason` (D17) names the cause.
     */
    async run(): Promise<boolean> {
        let lastProgress = monotonicNow();
        let crcErrors = 0;
        const v = this.verbose ? (m: string) => console.debug(`[session] ${m}`) : null;
        v?.(`run start — window=${this.window}`);

        for (;;) {
            if (this.truncated) break;

            // Done only when the source is finished AND every packet it produced
            // has been confirmed. The test is `_emitted`, not `_next`: after a
            // go-back `_next` rewinds to `_base` while the retained window still
            // holds pulled-but-unsent packets, so testing `_next` here strands
            // them and exits reporting success.
            if (this._done && this._base >= this._emitted) break;

            await this._fillWindow();

            // An open source with nothing in flight and nothing to send: wait
            // for input rather than spinning. Not a stall — the operator simply
            // is not pressing anything.
            if (this._next <= this._base) {
                if (this._done) break;
                await this._ctx.wait();
                continue;
            }

            const resp = await this._ackSink.get(ACK_TIMEOUT_MS);

            if (resp === null) {
                // silence
                if (monotonicNow() - lastProgress > STALL_TIMEOUT_MS) {
                    this.fatalReason = FATAL_STALL;
                    v?.(`FATAL: stall — no ACK/NACK for ${STALL_TIMEOUT_MS}ms (emitted=${this._emitted}, base=${this._base})`);
                    return false; // fatal — stalled
                }
                v?.(`silence ${ACK_TIMEOUT_MS}ms — go-back to base=${this._base}`);
                await this._goBack(0x00, 0);
                continue;
            }

            if (resp instanceof Ack) {
                const advanced = this._applyAck(resp.expectedSeq);
                if (advanced) lastProgress = monotonicNow();
                v?.(`ACK expectedSeq=${resp.expectedSeq} → base=${this._base} ${advanced ? "(advanced)" : "(duplicate)"}`);
                continue;
            }

            // Nack
            this.nacks++;
            const r = resp.reason;
            v?.(`NACK reason=0x${r.toString(16)} (${fatalReasonName(r)}) base=${this._base}`);
            if (r === NACK_BAD_MAGIC) {
                this.fatalReason = NACK_BAD_MAGIC;
                v?.(`FATAL: NACK_BAD_MAGIC`);
                return false; // fatal
            }
            if (r === NACK_PAUSED || r === NACK_BAD_STATE) {
                this.fatalReason = r;
                v?.(`FATAL: wrong machine state (${fatalReasonName(r)})`);
                return false; // fatal — wrong machine state
            }
            if (r === NACK_CRC) {
                crcErrors++;
                if (crcErrors > MAX_CRC_ERRORS) {
                    this.fatalReason = FATAL_CRC_LIMIT;
                    v?.(`FATAL: CRC errors ${crcErrors} > ${MAX_CRC_ERRORS}`);
                    return false; // fatal
                }
                await this._goBack(r, 0);
            } else {
                // NACK_FULL (backpressure) or NACK_ABORTING (§4.5 barrier):
                // wait for the ring to drain / the abort ramp to finish, then
                // resend from base. NACK_ABORTING is a barrier, not an error —
                // the Pico ramps to rest and lands IDLE; retry until it accepts.
                await this._goBack(r, BACKPRESSURE_MS);
            }
            lastProgress = monotonicNow();
        }

        v?.(`run done — emitted=${this._emitted} acked=${this.acked} truncated=${this.truncated}`);
        return true;
    }

    stats(): SessionStats {
        return {
            emitted: this._emitted,
            sent: this.sent,
            acked: this.acked,
            nacks: this.nacks,
            retries: this.retries,
            truncated: this.truncated,
        };
    }

    /** Stream outcome (D17) — `ok` plus the fatal reason and the stats tally. */
    result(): StreamResult {
        return {
            ok: this.fatalReason === undefined,
            fatalReason: this.fatalReason,
            emitted: this._emitted,
            sent: this.sent,
            acked: this.acked,
            nacks: this.nacks,
            retries: this.retries,
            truncated: this.truncated,
        };
    }

    // -- window management ----------------------------------------------------

    private async _fillWindow(): Promise<void> {
        const room = this.window - (this._next - this._base);
        if (room <= 0) return;

        // Retransmission first: these are already in _window, never re-pulled.
        let pending = this._window.slice(this._next - this._base);

        if (pending.length === 0 && !this._done) {
            this._ctx.room = room;
            this._ctx.inFlight = this._next - this._base;
            this._ctx.emitted = this._emitted;
            this._ctx.acked = this.acked;

            const batch = await this._source.pull(this._ctx);
            if (batch === null) {
                this._done = true;
                return;
            }
            for (const pkt of batch) {
                this._window.push(stampSeq(pkt, this._seq));
                this._seq = (this._seq + 1) & 0xff;
                this._emitted++;
            }
            pending = this._window.slice(this._next - this._base);
        }

        if (pending.length === 0) return;

        const written = await this._writer.writeBatch(pending.slice(0, room), this._abortFlag());
        this._next += written;
        this.sent += written;
    }

    private _abortFlag() {
        // The Writer wants an AbortToken; the session's abort is a promise, not
        // a flag. An arrow function captures `this` lexically (no `this` alias)
        // and reports set iff the session was truncated.
        const isTruncated = (): boolean => this.truncated;
        return { isSet: isTruncated };
    }

    /**
     * Advance on a cumulative ACK. The delta is computed in 8-bit rolling seq
     * space and CLAMPED to the in-flight window, so a duplicate ACK (delta 0)
     * or a stale/wrapped value can neither stall nor over-advance.
     */
    private _applyAck(expectedSeq: number): boolean {
        const baseSeq = (this._seq - (this._emitted - this._base)) & 0xff;
        const delta = (expectedSeq - baseSeq) & 0xff;
        if (delta > 0 && delta <= this._next - this._base) {
            this._base += delta;
            this.acked += delta;
            this._window.splice(0, delta); // release confirmed packets
            return true;
        }
        return false;
    }

    private async _goBack(_reason: number, backoffMs: number): Promise<void> {
        if (backoffMs > 0) await delay(backoffMs);
        this._next = this._base;
        this.retries++;
    }
}

const NACK_REASON_NAMES: Readonly<Record<number, string>> = {
    [NACK_CRC]: "NACK_CRC",
    [NACK_FULL]: "NACK_FULL",
    [NACK_BAD_MAGIC]: "NACK_BAD_MAGIC",
    [NACK_PAUSED]: "NACK_PAUSED",
    [NACK_BAD_STATE]: "NACK_BAD_STATE",
    [NACK_ABORTING]: "NACK_ABORTING",
    [FATAL_STALL]: "FATAL_STALL",
    [FATAL_CRC_LIMIT]: "FATAL_CRC_LIMIT",
};

/** Human-readable name for a `Session.fatalReason` value (D17). */
export function fatalReasonName(r: number): string {
    return NACK_REASON_NAMES[r] ?? `FATAL(0x${r.toString(16)})`;
}

/**
 * Stream outcome (D17) — `link.stream()` and `Session.run()` both surface WHY
 * a stream died, not just THAT it died. `ok` mirrors the boolean callers that
 * ignore the reason relied on (`if (await sess.run()) …`); `fatalReason` is set
 * only when `ok === false`. Stats are the running tally at exit, so a failed
 * stream shows how far it got (emitted/acked) before the failure.
 */
export interface StreamResult {
    readonly ok: boolean;
    readonly fatalReason?: number;
    readonly emitted: number;
    readonly sent: number;
    readonly acked: number;
    readonly nacks: number;
    readonly retries: number;
    readonly truncated: boolean;
}

function monotonicNow(): number {
    return typeof performance !== "undefined" ? performance.now() : Date.now();
}