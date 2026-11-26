/**
 * link/link.ts — owns a Transport, one Demux, one Writer, and the sink set
 * (docs/system/wire.md, docs/reference/host-api/link.md).
 *
 * Callers subscribe rather than seize: commands(), the status and session()
 * can all be in flight at once, because each awaits its own sink while the
 * single read loop keeps draining the Transport into the Demux. Nothing else
 * reads the Transport.
 *
 * The Link polls the status itself every `statusPollMs`, so `status` is never
 * more than one poll old and an open job always hears from the host.
 */

import type { Transport } from "../transport/index.js";
import type { Attachable } from "../transport/attach.js";
import type { Writable } from "./abort.js";
import { Writer } from "./writer.js";
import { Demux, makeSinks, JogAck, type DemuxSinks } from "./demux.js";
import type { Sink, LatestSink } from "./sink.js";
import { Ack } from "./demux.js";
import { Refusal, type Nack } from "../packets/nack.js";
import type { CfgNack } from "../packets/cfg.js";
import { Session, type JobSource } from "./session.js";
import { setTimer, clearTimer, type TimerId } from "./clock.js";
import { Commands } from "./commands.js";

import {
    CJOG_DEADMAN_MS,
    HOST_TIMEOUT_MS,
    MAGIC_ABORT,
    MAGIC_CJOG_STOP,
    MAGIC_CFG_ACK,
    MAGIC_CFG_DATA,
    MAGIC_CFG_GET,
    MAGIC_CFG_NACK,
    MAGIC_CFG_RDY,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
} from "../packets/constants.js";
import { packCjog, type JogDir } from "../packets/cjog.js";
import { CFG_MAX_BYTES, packCfgSetHeader } from "../packets/cfg.js";
import { crc32 } from "../packets/crc.js";
import { parseStatusRsp, type MachineStatus } from "../packets/status.js";
import { encodeConfig, decodeConfig } from "../config/blob.js";
import type { ControllerConfig } from "../config/schema.js";

/**
 * A config transfer that failed. `reason` is a CfgNack code, or null when
 * the controller did not answer or the failure is the host's.
 */
export class ConfigTransferError extends Error {
    constructor(
        message: string,
        readonly reason: number | null,
    ) {
        super(message);
        this.name = "ConfigTransferError";
    }
}

/** Counters since construction, for diagnostics. */
export interface LinkStats {
    /** Reply lines that arrived with no command waiting for them. Should stay 0. */
    readonly textDesyncs: number;
    readonly frames: number;
    readonly textLines: number;
    /** Bytes that started neither a frame nor a line. */
    readonly unknownBytes: number;
    /** Lines too long to buffer, dropped. */
    readonly overruns: number;
}

export interface LinkOptions {
    /** How often to ask for the status, ms. Default 100; 10 to 1000. */
    statusPollMs?: number;
}

/** A continuous jog's direction on each axis; absent is 0. One axis set per jog: XY, Z or A. */
export interface CJogDirection {
    x?: JogDir;
    y?: JogDir;
    z?: JogDir;
    a?: JogDir;
}

/** The controller's answer to a jog start: `reason` is a Nack code, or null for no answer in time. */
export type CJogReply = { ok: true } | { ok: false; reason: Nack | null };

const STATUS_POLL_MIN = 10;
/** At most a third of HOST_TIMEOUT_MS, so one lost request can't pause a job. */
const STATUS_POLL_MAX = Math.floor(HOST_TIMEOUT_MS / 3);
/** Resend the jog packet this often: a third of the deadman, so one lost packet is not a stop. */
/** How long `reset` waits for the boot banner to end (`ready`) after its `ok`, ms. */
const RESET_WAIT_MS = 5000;
const CJOG_REPEAT_MS = Math.floor(CJOG_DEADMAN_MS / 3);
/** How long to wait for the answer to a start; a turn brakes first, so it waits longer. */
const CJOG_START_TIMEOUT_MS = 1000;
const CJOG_TURN_TIMEOUT_MS = 5000;

type Dir4 = Required<CJogDirection>;

function checkDir(dir: CJogDirection): Dir4 {
    const d: Dir4 = { x: dir.x ?? 0, y: dir.y ?? 0, z: dir.z ?? 0, a: dir.a ?? 0 };
    for (const v of [d.x, d.y, d.z, d.a]) {
        if (v !== -1 && v !== 0 && v !== 1) throw new RangeError(`cjog direction ${String(v)} is not -1, 0 or 1`);
    }
    const sets = (d.x || d.y ? 1 : 0) + (d.z ? 1 : 0) + (d.a ? 1 : 0);
    if (sets !== 1) throw new RangeError("cjog moves exactly one axis set: XY, Z or A");
    return d;
}

const sameDir = (p: Dir4, q: Dir4): boolean => p.x === q.x && p.y === q.y && p.z === q.z && p.a === q.a;

/**
 * A continuous jog, from Link.cjog(). While it runs it resends the jog packet
 * faster than CJOG_DEADMAN_MS, on a clock that keeps time in a background tab.
 *
 * The controller answers only a start (JOG_ACK) or a refusal (JOG_NACK); a
 * repeat and a stop get no reply, so every reply on the jog sink belongs to
 * the oldest start still waiting.
 */
export class CJog {
    /** The controller's answer to the start. */
    readonly started: Promise<CJogReply>;
    private _running = true;
    private _dir: Dir4;
    private _packet: Uint8Array;
    private _timer: TimerId | null = null;

    /** @internal Use Link.cjog(). */
    constructor(
        private readonly _link: Link,
        dir: CJogDirection,
        private readonly _speed: number,
    ) {
        this._dir = checkDir(dir);
        if (!(_speed > 0)) throw new RangeError(`cjog speed ${_speed} is not above 0`);
        this._packet = this._pack(this._dir);
        this._link.sinks.jog.clear();
        this.started = this._send(CJOG_START_TIMEOUT_MS);
        this._schedule();
    }

    /** The jog is being kept alive; false after stop(), a refusal, or the link closing. */
    get running(): boolean {
        return this._running && !this._link.closed;
    }

    /**
     * Change direction. The machine comes to rest first, then sets off the new
     * way, so the answer arrives once it has stopped. The speed stays the jog's.
     * A refusal stops the jog.
     */
    turn(dir: CJogDirection): Promise<CJogReply> {
        const d = checkDir(dir);
        if (!this.running) throw new Error("cjog is not running");
        // The controller reads the same direction as a repeat and does not answer.
        if (sameDir(d, this._dir)) return Promise.resolve({ ok: true });
        this._dir = d;
        this._packet = this._pack(d);
        return this._send(CJOG_TURN_TIMEOUT_MS);
    }

    /** Stop the jog at once and stop repeating. No reply. */
    stop(): void {
        this._halt();
        void this._link.writer.writeFrame(new Uint8Array([MAGIC_CJOG_STOP])).catch(() => undefined);
    }

    /** @internal Stop repeating without telling the controller. */
    _halt(): void {
        this._running = false;
        if (this._timer !== null) clearTimer(this._timer);
        this._timer = null;
    }

    private _pack(d: Dir4): Uint8Array {
        return packCjog(d.x, d.y, this._speed, d.z, d.a);
    }

    private async _send(timeoutMs: number): Promise<CJogReply> {
        await this._link.writer.writeFrame(this._packet).catch(() => undefined);
        const r = await this._link.sinks.jog.get(timeoutMs);
        if (r instanceof JogAck) return { ok: true };
        // A refusal, or no answer in time: either way the jog is over.
        if (this._running) this.stop();
        return { ok: false, reason: r instanceof Refusal ? r.code : null };
    }

    private _schedule(): void {
        this._timer = setTimer(() => {
            this._timer = null;
            if (!this.running) {
                this._running = false;
                return;
            }
            void this._link.writer.writeFrame(this._packet).catch(() => undefined);
            this._schedule();
        }, CJOG_REPEAT_MS);
    }
}

export class Link {
    /** @internal */
    readonly sinks: DemuxSinks;
    /** @internal */
    readonly demux: Demux;
    /** @internal */
    readonly writer: Writer;
    private readonly _transport: Transport;
    private _textLock: Promise<unknown> = Promise.resolve();
    /** Who each awaited text line belongs to, in write order (_command). */
    private readonly _owed: { readonly kind: "reply" | "drop" }[] = [];
    private _commands: Commands | null = null;
    private _cfgLock: Promise<unknown> = Promise.resolve();
    private readonly _readLoop: Promise<void>;
    private readonly _statusPollMs: number;
    private _pollTimer: TimerId | null = null;
    private readonly _listeners = new Set<(s: MachineStatus) => void>();
    private _jog: CJog | null = null;
    private _onClose: () => void = () => undefined;
    private readonly _whenClosed: Promise<void>;

    /**
     * Orphaned text lines discarded by command(). Should stay 0 — anything
     * else means a command replied with more lines than it is allowed to, a
     * firmware contract bug worth surfacing rather than silently absorbing.
     */
    private _textDesyncs = 0;
    private _closed = false;
    /**
     * When true, sessions made after setting it log each ACK, NACK, go-back
     * and fatal at console.debug level. Default off.
     */
    verbose = false;

    constructor(transport: Transport, options: LinkOptions = {}) {
        const pollMs = options.statusPollMs ?? 100;
        if (!(pollMs >= STATUS_POLL_MIN && pollMs <= STATUS_POLL_MAX)) {
            throw new RangeError(
                `statusPollMs ${pollMs} outside ${STATUS_POLL_MIN}..${STATUS_POLL_MAX} ` +
                    `(a third of HOST_TIMEOUT_MS ${HOST_TIMEOUT_MS})`,
            );
        }
        this._statusPollMs = pollMs;
        this._whenClosed = new Promise<void>((r) => (this._onClose = r));
        this._transport = transport;
        this.sinks = makeSinks();
        this.demux = new Demux(this.sinks);
        this.writer = new Writer(transport as Writable);
        // A Sim-style backend feeds replies straight into the demux via its
        // own `write`; real-port backends pump bytes through `read()` which
        // the _pump loop drains into the same demux. Either way, hand the
        // demux to the backend if it wants it.
        const attachable = transport as Transport & Partial<Attachable>;
        if (typeof attachable.attach === "function") {
            attachable.attach(this.demux);
        }
        this._readLoop = this._pump();
        void this._dispatchStatus();
        this._schedulePoll();
    }

    get closed(): boolean {
        return this._closed;
    }

    private async _pump(): Promise<void> {
        try {
            for await (const chunk of this._transport.read()) {
                this.demux.feed(chunk);
            }
        } catch {
            // read failed (port closed / unplugged) — nothing to do here; a
            // pending command/session resolves on its sink timeout, and the
            // next call sees `closed`.
        }
        this._closed = true;
        if (this._pollTimer !== null) clearTimer(this._pollTimer);
        this._pollTimer = null;
        this._jog?._halt();
        this._onClose();
    }

    /** Ask for the status every statusPollMs until the port closes. */
    private _schedulePoll(): void {
        this._pollTimer = setTimer(() => {
            this._pollTimer = null;
            if (this._closed) return;
            void this.writer.writeFrame(new Uint8Array([MAGIC_STATUS_REQ])).catch(() => undefined);
            this._schedulePoll();
        }, this._statusPollMs);
    }

    /**
     * Hand every status sample to the onStatus listeners, in arrival order. The
     * sink's stamp counts samples, so waiting for one newer than the last seen
     * misses none.
     */
    private async _dispatchStatus(): Promise<void> {
        let stamp = this.sinks.status.sample[1];
        const closed = this._whenClosed.then(() => null);
        for (;;) {
            const got = await Promise.race([this.sinks.status.waitUpdate(undefined, stamp), closed]);
            if (got === null) return;
            const [raw, at] = got;
            stamp = at;
            if (!raw || this._listeners.size === 0) continue;
            const s = parseStatusRsp(raw);
            for (const l of [...this._listeners]) {
                try {
                    l(s);
                } catch (e) {
                    console.error("onStatus listener threw", e);
                }
            }
        }
    }

    // -- in turn: text commands -----------------------------------------------

    /**
     * Send one control-plane line and return the reply line (stripped), or ""
     * on timeout. Text stays strictly one-outstanding: the next line in the
     * sink is ours, once the lines owed to an earlier `stop` are passed.
     *
     * Replies come back in write order, so `_owed` lists, in that order, who
     * each awaited line belongs to: a command, or an `estop()` whose `ok`
     * nobody reads. A line already in the sink when this command's turn comes
     * is either owed to an estop, or orphaned (its awaiter timed out, or the
     * firmware sent more lines than the contract allows); orphans count in
     * stats().textDesyncs.
     *
     * `tail`: after an `ok`, keep reading and drop lines up to and including
     * this one (`reset` and its boot banner).
     *
     * @internal Through Commands.
     */
    async _command(text: string, timeoutMs = 1000, tail?: string): Promise<string> {
        const run = this._textLock.then(async () => {
            this._dropStale();
            const mine = { kind: "reply" as const };
            this._owed.push(mine);
            await this.writer.writeText(text);
            let reply: string | null;
            for (;;) {
                reply = await this.sinks.text.get(timeoutMs);
                if (reply === null || this._owed[0] === mine) break;
                this._owed.shift(); // an estop's `ok`
            }
            const at = this._owed.indexOf(mine);
            if (at >= 0) this._owed.splice(at, 1);
            if (reply === null) return "";
            if (tail !== undefined && reply === "ok") {
                const until = Date.now() + RESET_WAIT_MS;
                for (;;) {
                    const line = await this.sinks.text.get(Math.max(1, until - Date.now()));
                    if (line === null || line === tail) break;
                }
            }
            return reply;
        });
        this._textLock = run.then(
            () => undefined,
            () => undefined,
        );
        return run as Promise<string>;
    }

    /** Drop the lines waiting in the text sink: an estop's, or orphans. */
    private _dropStale(): void {
        for (let n = this.sinks.text.clear(); n > 0; n--) {
            if (this._owed[0]?.kind === "drop") this._owed.shift();
            else this._textDesyncs++;
        }
    }

    // -- at once ----------------------------------------------------------------

    /**
     * The e-stop: sends the `stop` line at once, never behind a pending
     * command. No reply; the status shows it take hold (ESTOP, then ALARM).
     */
    estop(): void {
        this._owed.push({ kind: "drop" }); // its `ok`, which nobody reads
        void this.writer.writeText("stop").catch(() => undefined);
    }

    /** @internal Fire-and-forget text line: the writer, not a reply slot. */
    _send(text: string): Promise<void> {
        return this.writer.writeText(text);
    }

    /** The link's text commands: the same object on every call. */
    commands(): Commands {
        this._commands ??= new Commands(
            (line, timeoutMs, tail) => this._command(line, timeoutMs, tail),
            this,
        );
        return this._commands;
    }

    /** Latest status sample, at most one poll old, or null before the first. */
    get status(): MachineStatus | null {
        const raw = this.sinks.status.value;
        return raw ? parseStatusRsp(raw) : null;
    }

    /**
     * Wait for a status sample that arrives after the call. Sends nothing: the
     * poll brings one within statusPollMs. Rejects if the port closes first.
     */
    freshStatus(): Promise<MachineStatus> {
        if (this._closed) return Promise.reject(new Error("link closed"));
        const since = this.sinks.status.sample[1];
        return Promise.race([
            this.sinks.status.waitUpdate(undefined, since).then(([raw]) => parseStatusRsp(raw!)),
            this._whenClosed.then((): never => {
                throw new Error("link closed");
            }),
        ]);
    }

    /**
     * Call `listener` with every status sample until off() or the port closes.
     * Any number of listeners; off() stops only this one.
     */
    onStatus(listener: (s: MachineStatus) => void): () => void {
        const l = (s: MachineStatus): void => listener(s);
        this._listeners.add(l);
        return () => {
            this._listeners.delete(l);
        };
    }

    /**
     * Get the machine to IDLE, position kept: brake the planner's motion to
     * rest and drop what is queued; in a job, also lift, switch the tool off
     * and close the job. Sends the ABORT frame, which never waits behind a
     * command. No reply; records sent before IDLE get NACK_ABORTING, which the
     * session treats as a barrier, not an error.
     */
    abort(): void {
        void this.writer.writeFrame(new Uint8Array([MAGIC_ABORT])).catch(() => undefined);
    }

    /**
     * Start a continuous jog. The returned CJog repeats the packet while it
     * runs; its `started` says whether the controller took it. A jog already
     * running stops repeating, and the new packet takes over.
     */
    cjog(dir: CJogDirection, options: { speed?: number } = {}): CJog {
        this._jog?._halt();
        this._jog = new CJog(this, dir, options.speed ?? 1);
        return this._jog;
    }

    // -- config (docs/system/config.md) ---------------------------------------

    /**
     * Encode `config` and store it on the controller. Two-phase: the header
     * goes first and the encoded config only after CFG_RDY, so a size or state
     * refusal never leaves payload bytes on the wire. Resolves on CFG_ACK, once
     * the controller has written it to flash and read it back. Throws
     * ConfigTransferError on a NACK, no reply, or an encoded config too large.
     */
    pushConfig(config: ControllerConfig): Promise<{ bytes: number; crc32: number }> {
        return this._pushEncoded(encodeConfig(config));
    }

    /**
     * @internal pushConfig() past the encoding. `commitTimeoutMs` covers the
     * controller's decode, flash write and readback.
     */
    _pushEncoded(
        encoded: Uint8Array,
        timeoutMs = 1000,
        commitTimeoutMs = 5000,
    ): Promise<{ bytes: number; crc32: number }> {
        return this._withCfgLock(async () => {
            if (encoded.length === 0 || encoded.length > CFG_MAX_BYTES) {
                throw new ConfigTransferError(
                    `encoded config is ${encoded.length} bytes (1..${CFG_MAX_BYTES})`,
                    null,
                );
            }
            const crc = crc32(encoded);
            this.sinks.cfg.clear();
            await this.writer.writeFrame(packCfgSetHeader(encoded.length, crc));
            await this._cfgExpect(MAGIC_CFG_RDY, "header", timeoutMs);
            await this.writer.writeFrame(encoded);
            await this._cfgExpect(MAGIC_CFG_ACK, "commit", commitTimeoutMs);
            return { bytes: encoded.length, crc32: crc };
        });
    }

    /**
     * Fetch the stored config, or null when none is stored. Throws
     * ConfigTransferError on no reply, or stored bytes that fail their CRC or
     * don't decode.
     */
    async pullConfig(): Promise<ControllerConfig | null> {
        const encoded = await this._pullEncoded();
        if (encoded === null) return null;
        try {
            return decodeConfig(encoded);
        } catch (e) {
            throw new ConfigTransferError(`stored config does not decode: ${(e as Error).message}`, null);
        }
    }

    /** @internal pullConfig() before the decoding. */
    _pullEncoded(timeoutMs = 3000): Promise<Uint8Array | null> {
        return this._withCfgLock(async () => {
            this.sinks.cfg.clear();
            await this.writer.writeFrame(new Uint8Array([MAGIC_CFG_GET]));
            const r = await this.sinks.cfg.get(timeoutMs);
            if (!r || r.kind !== MAGIC_CFG_DATA) {
                throw new ConfigTransferError("no CFG_DATA reply", null);
            }
            const payload = r.payload ?? new Uint8Array(0);
            if (payload.length === 0) return null;
            if (crc32(payload) !== r.crc32) {
                throw new ConfigTransferError("pulled config failed its CRC32", null);
            }
            return payload;
        });
    }

    private _withCfgLock<T>(fn: () => Promise<T>): Promise<T> {
        const run = this._cfgLock.then(fn);
        this._cfgLock = run.then(
            () => undefined,
            () => undefined,
        );
        return run;
    }

    private async _cfgExpect(kind: number, phase: string, timeoutMs: number): Promise<void> {
        const r = await this.sinks.cfg.get(timeoutMs);
        if (!r) throw new ConfigTransferError(`no reply to config ${phase}`, null);
        if (r.kind === MAGIC_CFG_NACK) {
            throw new ConfigTransferError(
                `config ${phase} refused (reason 0x${(r.reason ?? 0).toString(16)})`,
                (r.reason ?? null) as CfgNack | null,
            );
        }
        if (r.kind !== kind) {
            throw new ConfigTransferError(
                `unexpected reply 0x${r.kind.toString(16)} to config ${phase}`,
                null,
            );
        }
    }

    // -- jobs ---------------------------------------------------------------------

    /**
     * @internal Align the Pico's expectedSeq with a fresh seq counter (the
     * tests' raw frame sender; a session resets its own). The ACK is drained here, so a
     * sender opening on a stale ACK would not advance against a frame
     * it never sent.
     */
    async resetSeq(timeoutMs = 1000): Promise<boolean> {
        this.sinks.ack.clear();
        await this.writer.writeFrame(new Uint8Array([MAGIC_SEQRESET]));
        const r = await this.sinks.ack.get(timeoutMs);
        return r !== null;
    }

    /**
     * A session for `job`. Nothing is sent until its run(), which asks the
     * controller first: it sends the job to an idle controller, or takes over
     * the same job paused there (docs/reference/host-api/session.md).
     */
    session(job: JobSource): Session {
        const sess = new Session(this.writer, this.sinks.ack, this.sinks.status, job,
            (line) => this._command(line), this._whenClosed);
        sess.verbose = this.verbose;
        return sess;
    }

    /** Counters since construction, for diagnostics. */
    stats(): LinkStats {
        return { textDesyncs: this._textDesyncs, ...this.demux.stats() };
    }

    async close(): Promise<void> {
        await this._transport.close();
    }
}

export type { Sink, LatestSink, Ack, Refusal };
