/**
 * link/session.ts — a job on the controller (docs/system/jobs.md; the
 * reference in docs/reference/host-api/session.md).
 *
 * A JobSource holds a job's frames; a Session sends them Go-Back-N over the
 * demux and follows the job until it closes. The controller takes records only
 * inside a job, so a session is always a job's: there is no plain stream.
 *
 * The session reads the Link's status poll and never asks for status itself.
 */

import type { Sink, LatestSink } from "./sink.js";
import type { Writer } from "./writer.js";
import {
    NACK_BAD_MAGIC,
    NACK_PAUSED,
    NACK_BAD_STATE,
    NACK_CRC,
    NACK_BAD_CURVE,
    NACK_SOFT_LIMIT,
    NACK_JOB,
    NACK_TOOL_UNKNOWN,
    NACK_TOOL_CHANGE,
    NACK_TOOL_UNAVAILABLE,
    NACK_OUT_OF_RANGE,
    NACK_NOT_HOMED,
    JOB_MAX_TOOLS,
    MAGIC_ABORT,
    MAGIC_JOB_HDR,
    MAGIC_SEQRESET,
} from "../packets/constants.js";
import { stampSeq } from "../packets/packet.js";
import { crc32 } from "../packets/crc.js";
import { delay } from "./clock.js";
import { packJobHdr, packLayerHdr, packToolHdr, type JobLayer, type ToolBox, type ToolHdr } from "../packets/layer.js";
import { Ack, type Nack } from "./demux.js";
import { nackName } from "./names.js";
import { JobReason, MachineContext, MachineState, NO_LAYER, parseStatusRsp, type MachineStatus } from "../packets/status.js";

const WINDOW = 16; // frames in flight
const ACK_TIMEOUT_MS = 200; // per-response wait before assuming loss
const STALL_TIMEOUT_MS = 3000; // silence before the session holds `silent`
const BACKPRESSURE_MS = 50; // wait for the ring to drain on NACK_FULL
const MAX_CRC_ERRORS = 20;
const IDLE_MS = 100; // a waiting session's nap between looks at the status
const STATUS_WAIT_MS = 1500; // the wait for the link's next status sample (polls are ≤ 1000 ms apart)
const STATUS_MISSES = 3; // samples missed in a row before a held job counts as lost to silence
const SEQRESET_WAIT_MS = 1000;

/** Why a session isn't sending (see Session.hold). */
export type Hold = "place" | "tool" | "paused" | "refused" | "silent" | "release";

/** Why a session failed: the controller refused the job, or the host gave up. */
export type Fatal =
    | { readonly by: "controller"; readonly nack: number; readonly detail: number }
    | { readonly by: "host"; readonly why: "stall" | "crc-limit" | "job-lost" | "job-mismatch" };

export interface SessionStats {
    /** Frames written, resends included. */
    readonly sent: number;
    readonly acked: number;
    readonly nacks: number;
    readonly retries: number;
}

/** What Session.run() resolves to. */
export interface SessionResult extends SessionStats {
    readonly ok: boolean;
    /** Set when ok is false. */
    readonly fatal?: Fatal;
    /** The job's last layer and record seen in the status, for a lost job. */
    readonly lastLayer?: number;
    readonly lastRecord?: number;
}

// ── the job's frames ──────────────────────────────────────────────────────────

/** @internal Frames handed out in order, no more than `room` at a time; null once all have gone. */
export interface Frames {
    pull(room: number): Uint8Array[] | null;
}

/**
 * @internal The job id: a CRC32 over the frames that open and make up the job,
 * in the order they are sent, leaving out each frame's seq (byte 2) and CRC8
 * and the job header's own id field (bytes 3..6).
 */
export function jobIdOf(frames: readonly Uint8Array[]): number {
    let c = 0;
    for (const f of frames) {
        const end = f.length - 1;
        c = crc32(f, 0, 2, c);
        c = crc32(f, f[0] === MAGIC_JOB_HDR ? 7 : 3, end, c);
    }
    return c;
}

function unionBox(a: ToolBox, b: ToolBox): ToolBox {
    return {
        minX: Math.min(a.minX, b.minX), minY: Math.min(a.minY, b.minY),
        maxX: Math.max(a.maxX, b.maxX), maxY: Math.max(a.maxY, b.maxY),
    };
}

/** A frame and the index of the layer it belongs to; -1 for preflight. */
interface LayerFrame {
    readonly packet: Uint8Array;
    readonly layer: number;
}

/** A job's frames, stopping at the first layer `allowed` refuses. */
class LayerStream implements Frames {
    private _i = 0;

    constructor(
        private readonly _frames: readonly LayerFrame[],
        private readonly _allowed: (layer: number) => boolean,
    ) {}

    get drained(): boolean {
        return this._i >= this._frames.length;
    }

    get nextLayer(): number {
        return this._frames[this._i]?.layer ?? -1;
    }

    pull(room: number): Uint8Array[] | null {
        if (this.drained) return null;
        const out: Uint8Array[] = [];
        const take = Math.max(1, room);
        while (out.length < take && !this.drained && (this.nextLayer < 0 || this._allowed(this.nextLayer))) {
            out.push(this._frames[this._i++]!.packet);
        }
        return out;
    }
}

/** A job's frames, built from its layers; each tool's header carries the bbox over all its layers. */
export class JobSource {
    /** The id the job header carries: the same layers give the same id. */
    readonly jobId: number;
    private readonly _layers: readonly JobLayer[];
    private readonly _tools: readonly ToolHdr[];

    /** Hold each layer back until release(). May change mid-job. */
    manual: boolean;
    /** Layers below this index may be sent in manual mode. */
    private _released = 0;
    private _stream: LayerStream | null = null;

    constructor(layers: readonly JobLayer[], options: { manual?: boolean } = {}) {
        this.manual = options.manual ?? false;
        const tools = new Map<number, ToolHdr>();
        for (const l of layers) {
            const t = tools.get(l.tool);
            if (t && t.head !== l.head) throw new RangeError(`tool ${l.tool} on heads ${t.head} and ${l.head}`);
            tools.set(l.tool, { tool: l.tool, head: l.head, bbox: t ? unionBox(t.bbox, l.bbox) : l.bbox });
        }
        if (tools.size < 1 || tools.size > JOB_MAX_TOOLS) throw new RangeError(`a job has 1..${JOB_MAX_TOOLS} tools, not ${tools.size}`);
        this._layers = layers;
        this._tools = [...tools.values()];
        this.jobId = jobIdOf(this._frames(0).map((f) => f.packet));
    }

    /** Manual mode: let the next unsent layer go. */
    release(): void {
        if (this._stream && !this._stream.drained) this._released = Math.max(this._released, this._stream.nextLayer + 1);
    }

    /** @internal How many frames open the job. The session sends no more until placement signs off. */
    get preflightFrames(): number {
        return 1 + this._tools.length;
    }

    /** @internal Whether `layer` is one of the job's. */
    hasLayer(layer: number): boolean {
        return this._layers.some((l) => (l.hdr.layer ?? 0) === layer);
    }

    /** @internal Manual mode: the next unsent layer is waiting for release(). */
    get waiting(): boolean {
        const s = this._stream;
        return this.manual && !!s && !s.drained && s.nextLayer >= this._released;
    }

    /** @internal Every frame has been handed out. */
    get drained(): boolean {
        return this._stream?.drained ?? false;
    }

    /** @internal The job from its start. */
    start(): Frames {
        return this._open(this._frames(this.jobId));
    }

    /** @internal A resume: this layer's header alone, its records from `record`, then the layers after it. */
    from(layer: number, record: number): Frames {
        const i = this._layers.findIndex((l) => (l.hdr.layer ?? 0) === layer);
        const l = this._layers[i];
        const first = l?.hdr.record ?? 0;
        if (!l || record < first || record > first + l.records.length) return this._open([]);
        // The layer the job stopped in has gone already; the ones after wait as before.
        this._released = Math.max(this._released, i + 1);
        return this._open([
            ...[packLayerHdr({ ...l.hdr, layer, record }), ...l.records.slice(record - first)].map((packet) => ({ packet, layer: i })),
            ...this._layers.flatMap((n, k) => (k > i ? this._newLayer(n).map((packet) => ({ packet, layer: k })) : [])),
        ]);
    }

    /** Every frame of the job from its start, the header carrying `id`. */
    private _frames(id: number): LayerFrame[] {
        const recordCount = this._layers.reduce((n, l) => n + l.records.length, 0);
        const preflight = [
            packJobHdr({ id, recordCount, thickness: this._layers[0]!.hdr.thickness, toolCount: this._tools.length }),
            ...this._tools.map((t) => packToolHdr(t)),
        ];
        return [
            ...preflight.map((packet) => ({ packet, layer: -1 })),
            ...this._layers.flatMap((l, i) => this._newLayer(l).map((packet) => ({ packet, layer: i }))),
        ];
    }

    private _open(frames: readonly LayerFrame[]): LayerStream {
        this._stream = new LayerStream(frames, (layer) => !this.manual || layer < this._released);
        return this._stream;
    }

    private _newLayer(l: JobLayer): Uint8Array[] {
        const t = this._tools.find((x) => x.tool === l.tool)!;
        return [packLayerHdr(l.hdr), packToolHdr({ ...t, head: l.head }), ...l.records];
    }
}

// ── the session ───────────────────────────────────────────────────────────────

/** The NACKs that mean the job, or the machine's state, is wrong. */
const FATAL_NACKS = new Set<number>([
    NACK_BAD_MAGIC, NACK_BAD_CURVE, NACK_SOFT_LIMIT, NACK_TOOL_UNKNOWN, NACK_TOOL_UNAVAILABLE,
    NACK_OUT_OF_RANGE, NACK_NOT_HOMED, NACK_BAD_STATE,
]);

/** Thrown inside run() to end it; caught there. */
class Ended {
    constructor(readonly fatal: Fatal | undefined) {}
}

export class Session {
    private readonly _writer: Writer;
    private readonly _ackSink: Sink<Ack | Nack>;
    private readonly _statusSink: LatestSink<Uint8Array>;
    private readonly _job: JobSource;
    private readonly _command: (line: string) => Promise<string>;
    private _source: Frames = { pull: () => null };
    /** @internal Log each ACK, NACK, resend and fatal to console.debug. */
    verbose = false;

    // Retransmit buffer: frames emitted but not yet cumulatively ACKed.
    // Separate from the source, so a resend never pulls a frame twice.
    private _window: Uint8Array[] = [];
    private _base = 0; // frames confirmed accepted
    private _next = 0; // frames handed to the writer
    private _emitted = 0; // frames pulled from the source
    private _seq = 0; // rolling 8-bit wire seq, stamped at emit time
    private _done = false; // the source returned null

    private _jobSeen = false; // a status has shown JOB
    private _openedStamp: number | undefined; // the status stamp at the first ACK
    private _placed = false; // placement signed off: layers may go
    private _ending = false; // close() or abort() asked
    private _dropped = false; // _drop() asked
    private _linkClosed = false; // the port closed: run() rejects
    private _go = false; // resume() asked
    private _pauseAsked = false; // pause() got `ok`
    private _staleStamp = 0; // status samples up to this stamp predate the last ACK or hold
    private _wakeResolve: (() => void) | null = null;
    private _held: Hold | null = null;
    private _sent = 0;
    private _acked = 0;
    private _retries = 0;
    private _nacks = 0;
    private _lastLayer: number | undefined;
    private _lastRecord: number | undefined;

    /** @internal Made by Link.session(). */
    constructor(
        writer: Writer,
        ackSink: Sink<Ack | Nack>,
        statusSink: LatestSink<Uint8Array>,
        job: JobSource,
        command: (line: string) => Promise<string>,
        closed: Promise<void>,
    ) {
        this._writer = writer;
        this._ackSink = ackSink;
        this._statusSink = statusSink;
        this._job = job;
        this._command = command;
        void closed.then(() => {
            this._linkClosed = true;
            this._wake();
        });
    }

    /** Why the session isn't sending; null while it sends or has sent everything. */
    get hold(): Hold | null {
        if (this._held) return this._held;
        return !this._ending && this._next <= this._base && this._job.waiting ? "release" : null;
    }

    /** Every frame is acknowledged; the machine may still be cutting. */
    get drained(): boolean {
        return this._done && this._base >= this._emitted;
    }

    stats(): SessionStats {
        return { sent: this._sent, acked: this._acked, nacks: this._nacks, retries: this._retries };
    }

    /**
     * Send `cancel`, which closes a job at rest; run() then resolves ok.
     * Resolves to the reply: refused while moving, the session carries on and
     * abort() is the way to stop.
     */
    async close(): Promise<string> {
        const ending = this._ending;
        this._ending = true;
        this._wake();
        const reply = await this._command("cancel");
        if (!reply.startsWith("ok")) this._ending = ending;
        return reply;
    }

    /**
     * Send `pause`; on `ok` the session holds `paused` once its in-flight
     * replies are in. Resolves to the reply. A pause sent elsewhere is still
     * found, by its NACK or the status.
     */
    async pause(): Promise<string> {
        const reply = await this._command("pause");
        if (reply.startsWith("ok")) {
            this._pauseAsked = true;
            this._wake();
        }
        return reply;
    }

    /** Send ABORT: brake if moving, lift Z once, close the job; run() then resolves ok. No reply. */
    abort(): void {
        this._ending = true;
        this._wake();
        void this._writer.writeFrame(new Uint8Array([MAGIC_ABORT]));
    }

    /**
     * @internal Stop following the job and send nothing more, leaving it open
     * on the controller, as a page reload would; run() then resolves ok.
     */
    _drop(): void {
        this._dropped = true;
        this._wake();
    }

    /**
     * Leave a hold: `paused` or `refused` at the status's resume point, `tool`
     * by resending the refused layer once the operator has mounted and zeroed.
     * False if not held so, if the machine is not in JOB (a jog or probe inside
     * the job), or if a tool hold is not yet in TOOL_WAIT.
     */
    resume(): boolean {
        const st = this._status();
        if (st?.state !== MachineState.JOB) return false;
        if (this._held === "tool") {
            if (st.reason !== JobReason.TOOL_WAIT) return false;
        } else if (this._held !== "paused" && this._held !== "refused") {
            return false;
        }
        this._go = true;
        this._wake();
        return true;
    }

    /**
     * Send the job, or take over the same job paused on the controller, and
     * follow it until it leaves the controller. Rejects if the link closes
     * first; the job stays open on the controller.
     */
    async run(): Promise<SessionResult> {
        let fatal: Fatal | undefined;
        try {
            await this._run();
        } catch (e) {
            if (!(e instanceof Ended)) throw e;
            fatal = e.fatal;
        }
        this._held = null;
        if (fatal) this._log(`FATAL: ${fatal.by === "host" ? fatal.why : nackName(fatal.nack)}`);
        return {
            ok: !fatal,
            ...(fatal && { fatal }),
            lastLayer: this._lastLayer,
            lastRecord: this._lastRecord,
            ...this.stats(),
        };
    }

    private _log(m: string): void {
        if (this.verbose) console.debug(`[session] ${m}`);
    }

    private async _run(): Promise<void> {
        let lastProgress = monotonicNow();
        let crcErrors = 0;

        // Ask the controller first: a job already open is ours to take over, or
        // not ours. The second sample is the first surely asked for after run().
        await this._nextStatus();
        const first = await this._nextStatus();
        if (first?.context === MachineContext.JOB) {
            await this._takeOver();
            this._placed = true;
            await this._hold("paused");
        } else {
            if (!(await this._seqReset())) throw new Ended({ by: "host", why: "stall" });
            this._source = this._job.start();
        }

        // Each hold returns once it is left, else throws Ended.
        const hold = async (h: Hold): Promise<void> => {
            await this._hold(h);
            lastProgress = monotonicNow();
        };

        for (;;) {
            if (!this._ending && !this._dropped) await this._fillWindow();
            this._checkEnded(this._status());
            if (this._ending) {
                await this._sleep(IDLE_MS);
                continue;
            }
            // A pause after the last ACK has no NACK to bring it: pause() says
            // so, and a pause sent elsewhere shows in the status. Only a sample
            // newer than the last ACK counts: an older one predates the header.
            const st = this._status();
            const fresh = this._statusSink.sample[1] > this._staleStamp;
            if (this._pauseAsked || (this._next <= this._base && fresh &&
                st?.state === MachineState.JOB && st.reason === JobReason.PAUSED)) {
                await hold("paused");
                continue;
            }

            if (!this._placed && this._base >= this._job.preflightFrames) {
                await hold("place");
                this._placed = true;
                continue;
            }

            if (this._next <= this._base) {
                // All taken, or a layer waits for release(): the job runs on until the host ends it.
                await this._sleep(IDLE_MS);
                continue;
            }

            const resp = await this._ackSink.get(ACK_TIMEOUT_MS);
            if (resp === null) {
                if (monotonicNow() - lastProgress > STALL_TIMEOUT_MS) {
                    await hold("silent");
                    continue;
                }
                this._goBack();
                continue;
            }
            if (resp instanceof Ack) {
                if (this._applyAck(resp.expectedSeq)) {
                    lastProgress = monotonicNow();
                    this._staleStamp = this._statusSink.sample[1];
                }
                continue;
            }

            this._nacks++;
            const r = resp.reason;
            this._log(`NACK ${nackName(r)} base=${this._base}`);
            if (this._ending) continue;
            // A preflight refusal has already ended the job on the controller.
            const refusedPreflight = r === NACK_JOB && this._base < this._job.preflightFrames;
            if (FATAL_NACKS.has(r) || refusedPreflight) {
                throw new Ended({ by: "controller", nack: r, detail: resp.detail });
            }
            const held: Hold | null =
                r === NACK_PAUSED ? "paused" :
                r === NACK_JOB ? "refused" : // a resume header off the resume point
                r === NACK_TOOL_CHANGE ? "tool" : null;
            if (held) {
                await hold(held);
                continue;
            }
            if (r === NACK_CRC) {
                if (++crcErrors > MAX_CRC_ERRORS) throw new Ended({ by: "host", why: "crc-limit" });
                this._goBack();
            } else {
                // NACK_FULL (backpressure), NACK_BUSY or NACK_ABORTING (the
                // abort barrier): wait, then resend from base.
                await delay(BACKPRESSURE_MS);
                this._goBack();
            }
            lastProgress = monotonicNow();
        }
    }

    /**
     * Take over the job open on the controller: it must be this job, on one of
     * its layers. A job still cutting is waited on until it pauses: the
     * controller pauses a dry job whose host has gone. Sends only `get jobid`.
     */
    private async _takeOver(): Promise<void> {
        const mismatch = new Ended({ by: "host", why: "job-mismatch" });
        let idChecked = false;
        let misses = 0;
        for (;;) {
            const st = await this._nextStatus();
            this._checkGone();
            if (!st) {
                if (++misses >= STATUS_MISSES) throw new Ended({ by: "host", why: "stall" });
                continue;
            }
            misses = 0;
            if (st.context !== MachineContext.JOB) throw new Ended({ by: "host", why: "job-lost" });
            if (st.layer === undefined || !this._job.hasLayer(st.layer)) throw mismatch;
            // Cutting, or on its way to the pause: braking, lifting (a state
            // other than JOB inside the job), or a dwell.
            const cutting = st.state !== MachineState.JOB || st.reason === JobReason.BUFFERING ||
                st.reason === JobReason.RUNNING || st.reason === JobReason.HOLDING || st.reason === JobReason.DWELL;
            const paused = st.state === MachineState.JOB && st.reason === JobReason.PAUSED;
            if (!paused && !cutting) throw mismatch;
            if (!idChecked) {
                const m = /\bjobid=([0-9a-f]{8})\b/.exec(await this._command("get jobid"));
                if (!m || parseInt(m[1]!, 16) !== this._job.jobId) throw mismatch;
                idChecked = true;
            }
            if (paused) {
                this._jobSeen = true;
                return;
            }
        }
    }

    /** Throws once the session can follow the job no more: dropped, or the link closed. */
    private _checkGone(): void {
        if (this._linkClosed) throw new Error("the link closed");
        if (this._dropped) throw new Ended(undefined);
    }

    /** Throws Ended once the job context has closed; throws as _checkGone(). */
    private _checkEnded(st: MachineStatus | null): void {
        this._checkGone();
        if (!st || st.context === undefined) return;
        if (st.context === MachineContext.JOB) {
            this._jobSeen = true;
            if (st.layer !== undefined && st.layer !== NO_LAYER && this._job.hasLayer(st.layer)) {
                this._lastLayer = st.layer;
                this._lastRecord = st.record;
            }
            return;
        }
        if (this._ending) throw new Ended(undefined);
        const opened = this._openedStamp !== undefined && this._statusSink.sample[1] > this._openedStamp;
        if (this._jobSeen || opened) throw new Ended({ by: "host", why: "job-lost" });
        // Not yet opened.
    }

    /**
     * Send nothing until the hold is left; throws Ended if the job ends. A
     * paused, refused or silent job must still be JOB on one of the job's
     * layers; anything else is a lost job, and resending to it would be ACKed
     * as stale duplicates without running (a rebooted controller).
     */
    private async _hold(h: Hold): Promise<void> {
        this._held = h;
        this._go = false;
        this._pauseAsked = false;
        this._log(`hold ${h}`);
        // The rest of the window answers too; let those replies land first.
        if (h !== "silent") await this._settle();
        try {
            for (;;) {
                const st = await this._nextStatus();
                this._checkEnded(st);
                if (!st) continue;
                if (this._ending) continue;
                if (h !== "tool" && h !== "place" && st.layer !== undefined && st.layer !== NO_LAYER &&
                    !this._job.hasLayer(st.layer)) {
                    throw new Ended({ by: "host", why: "job-lost" });
                }
                // A jog or probe inside the job: its reason is not the job's.
                if (st.state !== MachineState.JOB) continue;
                // Paused while the host was away (a lost host, or a pause sent
                // elsewhere): resending would only be refused. Hold for resume().
                if (h === "silent" && st.reason === JobReason.PAUSED) {
                    h = "paused";
                    this._held = h;
                    this._go = false;
                    this._log("hold silent → paused");
                    await this._settle();
                }
                if (await this._leave(h, st)) {
                    this._log(`leave ${h}`);
                    return;
                }
            }
        } finally {
            this._held = null;
            this._pauseAsked = false; // a pause() answered during this hold is spent
            this._staleStamp = this._statusSink.sample[1];
        }
    }

    /** Leave `h` if the status and the operator allow it, resending from where the job is. */
    private async _leave(h: Hold, st: MachineStatus): Promise<boolean> {
        switch (h) {
            case "place":
                return st.reason === JobReason.BUFFERING; // `place ok`
            case "silent":
                this._goBack();
                return true;
            case "tool": {
                const moving = st.reason === JobReason.TOOL_CHANGE;
                const waiting = st.reason === JobReason.TOOL_WAIT;
                // Resent by itself once the change is past; in TOOL_WAIT on the operator's word.
                if (moving || (waiting && !this._go)) return false;
                this._go = false;
                this._goBack();
                return true;
            }
            case "paused":
            case "refused":
            case "release":
                if (!this._go) return false;
                this._go = false;
                if (st.reason !== JobReason.PAUSED || st.layer === undefined || st.record === undefined) return false;
                return this._restart(st.layer, st.record);
        }
    }

    /** SEQRESET and its ACK. */
    private async _seqReset(): Promise<boolean> {
        this._ackSink.clear();
        await this._writer.writeFrame(new Uint8Array([MAGIC_SEQRESET]));
        const r = await this._ackSink.get(SEQRESET_WAIT_MS);
        return r instanceof Ack && r.expectedSeq === 0;
    }

    /** Drop the window and send the job again from (layer, record) behind a fresh seq. */
    private async _restart(layer: number, record: number): Promise<boolean> {
        if (!(await this._seqReset())) return false;
        this._window = [];
        this._emitted = this._base;
        this._next = this._base;
        this._seq = 0;
        this._done = false;
        this._source = this._job.from(layer, record);
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

    /** The latest status sample, or null. */
    private _status(): MachineStatus | null {
        const raw = this._statusSink.value;
        if (!raw) return null;
        try {
            return parseStatusRsp(raw);
        } catch {
            return null;
        }
    }

    /**
     * The link poll's next status sample, or null if none comes in time.
     * Woken early by close(), abort() or resume(), it returns the latest.
     */
    private async _nextStatus(): Promise<MachineStatus | null> {
        const sink = this._statusSink;
        const before = sink.sample[1];
        const woken = new Promise<boolean>((r) => {
            this._wakeResolve = () => r(true);
        });
        const wake = await Promise.race([sink.waitUpdate(STATUS_WAIT_MS, before).then(() => false), woken]);
        if (!wake && sink.sample[1] === before) return null;
        return this._status();
    }

    /** Wait `ms`, or less on close(), abort(), pause() or resume(). */
    private _sleep(ms: number): Promise<void> {
        const woken = new Promise<void>((r) => {
            this._wakeResolve = r;
        });
        return Promise.race([delay(ms), woken]);
    }

    private _wake(): void {
        this._wakeResolve?.();
        this._wakeResolve = null;
    }

    // -- window management ----------------------------------------------------

    private async _fillWindow(): Promise<void> {
        const room = WINDOW - (this._next - this._base);
        if (room <= 0) return;

        // Retransmission first: these are already in _window, never re-pulled.
        let pending = this._window.slice(this._next - this._base);

        // Nothing past the preflight frames until placement signs off.
        const gate = !this._placed ? this._job.preflightFrames - this._emitted : room;

        if (pending.length === 0 && !this._done && gate > 0) {
            const batch = this._source.pull(Math.min(room, gate));
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

        const written = await this._writer.writeBatch(pending.slice(0, room), { isSet: () => this._ending || this._dropped });
        this._next += written;
        this._sent += written;
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
            this._acked += delta;
            // An acked JOB_HDR opened the job: a sample after this one shows it.
            this._openedStamp ??= this._statusSink.sample[1];
            this._window.splice(0, delta); // release confirmed frames
            this._log(`ACK → base=${this._base}`);
            return true;
        }
        return false;
    }

    private _goBack(): void {
        this._next = this._base;
        this._retries++;
    }
}

function monotonicNow(): number {
    return typeof performance !== "undefined" ? performance.now() : Date.now();
}
