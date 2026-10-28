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
import {
    NACK_BAD_MAGIC,
    NACK_PAUSED,
    NACK_BAD_STATE,
    NACK_CRC,
    NACK_FULL,
    NACK_ABORTING,
    NACK_BAD_CURVE,
    NACK_SOFT_LIMIT,
    NACK_JOB,
    NACK_TOOL_UNKNOWN,
    NACK_TOOL_CHANGE,
    NACK_BUSY,
    MAGIC_ABORT,
    MAGIC_LAYER_HDR,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
} from "../format/constants.js";
import { stampSeq } from "../format/packet.js";
import { packLayerHdr, type LayerHdr } from "../format/layer.js";
import { Ack, Nack } from "./demux.js";
import { JobReason, MachineState, NO_LAYER, parseStatusRsp, type MachineStatus } from "../format/status.js";

export const DEFAULT_WINDOW = 16;
const ACK_TIMEOUT_MS = 200; // per-response wait before assuming loss
const STALL_TIMEOUT_MS = 3000; // total silence before a fatal abort (a hold, in a job)
const BACKPRESSURE_MS = 50; // wait for the ring to drain on NACK_FULL
const TOOL_CHANGE_MS = 250; // retry while the job holds for a tool change
const MAX_CRC_ERRORS = 20;
const IDLE_WAIT_MS = 20; // bound on an open source's idle wait
const STATUS_POLL_MS = 100; // a job session's STATUS_REQ period
const STATUS_WAIT_MS = 200; // a held job session's wait for its STATUS_RSP
const SEQRESET_WAIT_MS = 1000;

// ── fatal reason codes (D17) ────────────────────────────────────────────────────
// run() returns false on any fatal NACK OR on a self-inflicted failure (total
// silence past STALL_TIMEOUT_MS, too many CRC retries). Before D17 those paths
// returned a bare `false` and the caller had no way to tell them apart — a
// machine that NACKed BAD_STATE vs one that simply went silent looked the same.
// `fatalReason` is set at each fatal exit and `fatalReasonName()` renders it.
//
// NACK reasons (0x01..0x08) are passed through unchanged; self-inflicted ones
// use the 0xE0.. range so they never collide with a future wire NACK.
export const FATAL_STALL = 0xe0; // no ACK/NACK past STALL_TIMEOUT_MS
export const FATAL_CRC_LIMIT = 0xe1; // NACK_CRC count exceeded MAX_CRC_ERRORS
export const FATAL_JOB_LOST = 0xe2; // a job session's job left JOB without its cancel or end

/** Why a job session sends nothing (see Session.held). */
export type Hold = "paused" | "tool" | "refused" | "silent";

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

/**
 * A job's packets from any point, header first: what a job session restarts
 * from on a resume. The session never indexes records itself, so a source
 * that reads a job file can seek instead of holding the packets.
 */
export interface ReplayableSource {
    /** The job from its start. */
    start(): PacketSource;
    /** From `record` of `layer` on, behind a header naming that point. */
    from(layer: number, record: number): PacketSource;
}

/** One layer in memory: its header's fields and its BEZIER packets, record n = packets[n - hdr.record]. */
export class JobSource implements ReplayableSource {
    private readonly _hdr: LayerHdr;
    private readonly _packets: readonly Uint8Array[];

    constructor(hdr: LayerHdr, packets: readonly Uint8Array[]) {
        this._hdr = hdr;
        this._packets = packets;
    }

    start(): PacketSource {
        return this.from(this._hdr.layer ?? 0, this._hdr.record ?? 0);
    }

    from(layer: number, record: number): PacketSource {
        const first = this._hdr.record ?? 0;
        if (layer !== (this._hdr.layer ?? 0) || record < first) return new ListSource([]);
        return new ListSource([packLayerHdr({ ...this._hdr, layer, record }), ...this._packets.slice(record - first)]);
    }
}

function isReplayable(s: PacketSource | ReplayableSource): s is ReplayableSource {
    return "from" in s && "start" in s;
}

// ── the session ───────────────────────────────────────────────────────────────

export interface SessionStats {
    readonly held: Hold | null;
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
    private _source: PacketSource;
    private readonly _statusSink: LatestSink<Uint8Array> | undefined;
    /** Set for a job session: run() then lives until the job leaves JOB. */
    private readonly _job: ReplayableSource | undefined;
    private readonly _command: ((line: string) => Promise<string>) | undefined;
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

    // Job session only.
    private _layer: number | undefined; // the last header's layer
    private _jobSeen = false; // a status has shown JOB
    private _ending = false; // cancel() or end() asked
    private _go = false; // resume() asked
    private _pauseAsked = false; // pause() got `ok`
    private _lastPoll = -Infinity;
    private _staleStamp = 0; // status samples up to this stamp predate the last ACK or hold
    private _wakeResolve: (() => void) | null = null;

    /** A job session's hold, null while it is sending or done. */
    held: Hold | null = null;
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
        source: PacketSource | ReplayableSource,
        statusSink?: LatestSink<Uint8Array>,
        window: number = DEFAULT_WINDOW,
        /** A job session's text command path, for cancel(). */
        command?: (line: string) => Promise<string>,
    ) {
        this._writer = writer;
        this._ackSink = ackSink;
        if (isReplayable(source)) {
            if (!statusSink) throw new Error("a job session needs the status sink");
            this._job = source;
            this._source = source.start();
        } else {
            this._source = source;
        }
        this._command = command;
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

    /** Job session: send `cancel`; run() returns true once the job leaves JOB. Resolves to the reply. */
    async cancel(): Promise<string> {
        if (!this._job || !this._command) throw new Error("cancel() needs a job session with a command path");
        this._ending = true;
        this._wake();
        return this._command("cancel");
    }

    /**
     * Job session: send `pause`; on `ok` the session holds `paused` once its
     * in-flight replies are in. Resolves to the reply. A pause sent elsewhere
     * is still found, by its NACK or the status.
     */
    async pause(): Promise<string> {
        if (!this._job || !this._command) throw new Error("pause() needs a job session with a command path");
        const reply = await this._command("pause");
        if (reply.startsWith("ok")) {
            this._pauseAsked = true;
            this._wake();
        }
        return reply;
    }

    /**
     * Job session: send ABORT, which brakes if moving and lifts Z once; run()
     * returns true once the job leaves JOB. ABORT has no reply.
     */
    end(): void {
        if (!this._job) throw new Error("end() needs a job session");
        this._ending = true;
        this._wake();
        void this._writer.writeFrame(new Uint8Array([MAGIC_ABORT]));
    }

    /**
     * Leave a hold: `paused` or `refused` at the status's resume point, `tool`
     * by resending the refused window once the operator has mounted and zeroed.
     * False if not held, or if a tool hold is not yet in TOOL_WAIT.
     */
    resume(): boolean {
        if (this.held === "tool") {
            if (this._ctx.status?.reason !== JobReason.TOOL_WAIT) return false;
        } else if (this.held !== "paused" && this.held !== "refused") {
            return false;
        }
        this._go = true;
        this._wake();
        return true;
    }

    /**
     * Drive the stream to completion. Returns true on success or truncation,
     * false on fatal — in which case `fatalReason` (D17) names the cause.
     * A job session returns only on cancel(), end(), truncate(), a fatal NACK or a
     * lost job; it holds (`held`) while the machine takes no records.
     */
    async run(): Promise<boolean> {
        if (this._job) return this._runJob();
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
            if (r === NACK_BAD_CURVE) {
                this.fatalReason = r;
                v?.(`FATAL: the Pico refused a curve`);
                return false; // fatal — the record or its framing is wrong
            }
            if (r === NACK_SOFT_LIMIT) {
                this.fatalReason = r;
                v?.(`FATAL: a record lies outside the soft range`);
                return false; // fatal — the job does not fit where it is placed
            }
            if (r === NACK_JOB || r === NACK_TOOL_UNKNOWN) {
                this.fatalReason = r;
                v?.(`FATAL: the Pico refused the layer (${fatalReasonName(r)})`);
                return false; // fatal — no job open, or the layer's tool cannot run
            }
            if (r === NACK_PAUSED || r === NACK_BAD_STATE) {
                this.fatalReason = r;
                v?.(`FATAL: wrong machine state (${fatalReasonName(r)})`);
                return false; // fatal — wrong machine state
            }
            if (r === NACK_TOOL_CHANGE) {
                // The header was not consumed: the job holds while the operator
                // changes the tool. Resend from it until it is ACKed.
                await this._goBack(r, TOOL_CHANGE_MS);
            } else if (r === NACK_CRC) {
                crcErrors++;
                if (crcErrors > MAX_CRC_ERRORS) {
                    this.fatalReason = FATAL_CRC_LIMIT;
                    v?.(`FATAL: CRC errors ${crcErrors} > ${MAX_CRC_ERRORS}`);
                    return false; // fatal
                }
                await this._goBack(r, 0);
            } else {
                // NACK_FULL (backpressure), NACK_BUSY or NACK_ABORTING (§4.5
                // barrier):
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

    private async _runJob(): Promise<boolean> {
        let lastProgress = monotonicNow();
        let crcErrors = 0;
        const v = this.verbose ? (m: string) => console.debug(`[session] ${m}`) : null;
        v?.(`job run start — window=${this.window}`);

        // Each hold returns null once it is left, else what run() returns.
        const hold = async (h: Hold): Promise<boolean | null> => {
            const r = await this._hold(h, v);
            lastProgress = monotonicNow();
            return r;
        };

        for (;;) {
            if (this.truncated) return true;

            if (!this._ending) await this._fillWindow();
            // After the packets, so the Pico answers it having taken them.
            if (monotonicNow() - this._lastPoll >= STATUS_POLL_MS) {
                this._lastPoll = monotonicNow();
                await this._writer.writeFrame(new Uint8Array([MAGIC_STATUS_REQ]));
            }
            const st = this._ctx.status;
            const end = this._ended(st);
            if (end !== undefined) return end;
            if (this._ending) {
                await this._sleep(STATUS_POLL_MS);
                continue;
            }
            // A pause after the last ACK has no NACK to bring it: pause() says
            // so, and a pause sent elsewhere shows in the status. Only a sample
            // newer than the last ACK counts: an older one predates the header.
            const fresh = this._statusSink!.sample[1] > this._staleStamp;
            if (this._pauseAsked || (this._next <= this._base && fresh &&
                st?.state === MachineState.JOB && st.reason === JobReason.PAUSED)) {
                const r = await hold("paused");
                if (r !== null) return r;
                continue;
            }

            if (this._next <= this._base) {
                // All taken: the job runs on until the host ends it.
                await this._sleep(STATUS_POLL_MS);
                continue;
            }

            const resp = await this._ackSink.get(ACK_TIMEOUT_MS);
            if (resp === null) {
                if (monotonicNow() - lastProgress > STALL_TIMEOUT_MS) {
                    const r = await hold("silent");
                    if (r !== null) return r;
                    continue;
                }
                await this._goBack(0x00, 0);
                continue;
            }
            if (resp instanceof Ack) {
                if (this._applyAck(resp.expectedSeq)) {
                    lastProgress = monotonicNow();
                    this._staleStamp = this._statusSink!.sample[1];
                }
                continue;
            }

            this.nacks++;
            const r = resp.reason;
            v?.(`NACK reason=0x${r.toString(16)} (${fatalReasonName(r)}) base=${this._base}`);
            if (this._ending) continue;
            if (r === NACK_BAD_MAGIC || r === NACK_BAD_CURVE || r === NACK_SOFT_LIMIT ||
                r === NACK_TOOL_UNKNOWN || r === NACK_BAD_STATE) {
                this.fatalReason = r;
                v?.(`FATAL: ${fatalReasonName(r)}`);
                return false; // the job, or the machine's state, is wrong
            }
            let held: Hold | null = null;
            if (r === NACK_PAUSED) held = "paused";
            else if (r === NACK_JOB) held = "refused"; // a resume header off the resume point
            else if (r === NACK_TOOL_CHANGE) held = "tool";
            if (held) {
                const out = await hold(held);
                if (out !== null) return out;
                continue;
            }
            if (r === NACK_CRC) {
                if (++crcErrors > MAX_CRC_ERRORS) {
                    this.fatalReason = FATAL_CRC_LIMIT;
                    return false;
                }
                await this._goBack(r, 0);
            } else {
                await this._goBack(r, BACKPRESSURE_MS);
            }
            lastProgress = monotonicNow();
        }
    }

    /** What run() returns once a job session's job has left JOB, else undefined. */
    private _ended(st: MachineStatus | null): boolean | undefined {
        if (!st) return undefined;
        if (st.state === MachineState.JOB) {
            this._jobSeen = true;
            return undefined;
        }
        if (this._ending) return true;
        if (!this._jobSeen) return undefined; // not yet opened
        this.fatalReason = FATAL_JOB_LOST;
        return false;
    }

    /**
     * Send nothing but STATUS_REQ until the hold is left: null then, else
     * what run() returns. A paused, refused or silent job must still be JOB on
     * the session's layer; anything else is a lost job, and resending to it
     * would be ACKed as stale duplicates without running (a rebooted Pico).
     */
    private async _hold(h: Hold, v: ((m: string) => void) | null): Promise<boolean | null> {
        this.held = h;
        this._go = false;
        this._pauseAsked = false;
        v?.(`hold ${h}`);
        // The rest of the window answers too; let those replies land first.
        if (h !== "silent") await this._settle();
        try {
            for (;;) {
                if (this.truncated) return true;
                const st = await this._pollStatus();
                if (st) {
                    const end = this._ended(st);
                    if (end !== undefined) return end;
                    if (!this._ending) {
                        if (h !== "tool" && this._layer !== undefined && st.layer !== undefined &&
                            st.layer !== NO_LAYER && st.layer !== this._layer) {
                            this.fatalReason = FATAL_JOB_LOST;
                            return false;
                        }
                        const left = await this._leave(h, st);
                        if (left) {
                            v?.(`leave ${h}`);
                            return null;
                        }
                    }
                }
                await this._sleep(STATUS_POLL_MS);
            }
        } finally {
            this.held = null;
            this._pauseAsked = false; // a pause() answered during this hold is spent
            this._staleStamp = this._statusSink!.sample[1];
        }
    }

    /** Leave `h` if the status and the operator allow it, resending from where the job is. */
    private async _leave(h: Hold, st: MachineStatus): Promise<boolean> {
        switch (h) {
            case "silent":
                await this._goBack(0x00, 0);
                return true;
            case "tool": {
                const moving = st.reason === JobReason.TOOL_CHANGE;
                const waiting = st.reason === JobReason.TOOL_WAIT;
                // Resent by itself once the change is past; in TOOL_WAIT on the operator's word.
                if (moving || (waiting && !this._go)) return false;
                this._go = false;
                await this._goBack(NACK_TOOL_CHANGE, 0);
                return true;
            }
            case "paused":
            case "refused":
                if (!this._go) return false;
                this._go = false;
                if (st.reason !== JobReason.PAUSED || st.layer === undefined || st.record === undefined) return false;
                return this._restart(st.layer, st.record);
        }
    }

    /** Drop the window and send the job again from (layer, record) behind a fresh seq. */
    private async _restart(layer: number, record: number): Promise<boolean> {
        this._ackSink.clear();
        await this._writer.writeFrame(new Uint8Array([MAGIC_SEQRESET]));
        const r = await this._ackSink.get(SEQRESET_WAIT_MS);
        if (!(r instanceof Ack) || r.expectedSeq !== 0) return false;
        this._window = [];
        this._emitted = this._base;
        this._next = this._base;
        this._seq = 0;
        this._done = false;
        this._source = this._job!.from(layer, record);
        return true;
    }

    /** Read replies until ACK_TIMEOUT_MS of quiet, applying ACKs. */
    private async _settle(): Promise<void> {
        for (;;) {
            const r = await this._ackSink.get(ACK_TIMEOUT_MS);
            if (r === null) return;
            if (r instanceof Ack) this._applyAck(r.expectedSeq);
        }
    }

    /** One STATUS_REQ and its reply, or null. */
    private async _pollStatus(): Promise<MachineStatus | null> {
        const sink = this._statusSink!;
        const before = sink.sample[1];
        this._lastPoll = monotonicNow();
        await this._writer.writeFrame(new Uint8Array([MAGIC_STATUS_REQ]));
        const [data] = await sink.waitUpdate(STATUS_WAIT_MS, before);
        if (!data || sink.sample[1] === before) return null;
        try {
            return parseStatusRsp(data);
        } catch {
            return null;
        }
    }

    /** Wait `ms`, or less on truncate(), cancel(), end() or resume(). */
    private _sleep(ms: number): Promise<void> {
        const woken = new Promise<void>((r) => {
            this._wakeResolve = r;
        });
        return Promise.race([delay(ms), this._abortPromise, woken]);
    }

    private _wake(): void {
        this._wakeResolve?.();
        this._wakeResolve = null;
    }

    stats(): SessionStats {
        return {
            held: this.held,
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
                if (pkt[0] === MAGIC_LAYER_HDR) this._layer = pkt[5]! | (pkt[6]! << 8);
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
    [NACK_BAD_CURVE]: "NACK_BAD_CURVE",
    [NACK_SOFT_LIMIT]: "NACK_SOFT_LIMIT",
    [NACK_JOB]: "NACK_JOB",
    [NACK_TOOL_UNKNOWN]: "NACK_TOOL_UNKNOWN",
    [NACK_TOOL_CHANGE]: "NACK_TOOL_CHANGE",
    [NACK_BUSY]: "NACK_BUSY",
    [FATAL_STALL]: "FATAL_STALL",
    [FATAL_CRC_LIMIT]: "FATAL_CRC_LIMIT",
    [FATAL_JOB_LOST]: "FATAL_JOB_LOST",
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