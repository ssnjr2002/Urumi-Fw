/**
 * link/backends/sim.ts — in-process fake Pico (a Transport).
 * Ported from host/protocol/link.py SimBackend.
 *
 * Answers text commands per the wire_protocol.md allowed-state matrix AND
 * "executes" streamed data-plane packets: their step deltas integrate into the
 * tracked position over time while state holds RUNNING, so a job/jog actually
 * moves and finishes (returning to IDLE, or to PAUSED for a jog issued during a
 * pause). pause holds the executor; resume continues it; stop/cancel flush the
 * remaining motion. A test double — not real-time accurate.
 *
 * This is the backend that lets vitest exercise backpressure, coalesced ACKs,
 * open sessions, jog blend/cancel, status-during-stream and abort — all
 * without hardware. The SimTransport implements the Transport interface, so a
 * Link constructed over it works identically to one over a real port.
 *
 * Threading-vs-async: Python's SimBackend runs the executor on a daemon thread
 * under an RLock. The browser/Node are single-threaded, so the executor is a
 * setInterval timer and "the lock" is simply JS's run-to-completion: every
 * write() and every executor tick runs atomically with respect to each other,
 * because they cannot interleave on one event loop.
 */

import type { Demux } from "../demux.js";
import type { Transport } from "../transport.js";
import {
    MAGIC_ACK,
    MAGIC_NACK,
    MAGIC_ABORT,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
    NACK_FULL,
    NACK_BAD_STATE,
} from "../../format/constants.js";
import {
    MachineState,
    AlarmReason,
    RunningReason,
    axisMask,
    packStatusRsp,
} from "../../format/status.js";
import { unpackMicrosegment } from "../../format/packet.js";
import type { MicroSegment } from "../../format/microsegment.js";

const MSEG_FLAG_PAUSE = 0x04; // sender-inserted at a tool-change boundary

interface SimOptions {
    ringSize?: number;
    ackCoalesceMax?: number;
    frameMs?: number;
    fCpu?: number;
}

export class SimTransport implements Transport {
    readonly ringSize: number;
    readonly ackCoalesceMax: number;
    readonly frameMs: number;
    readonly fCpu: number;

    state: MachineState = MachineState.IDLE;
    alarm: AlarmReason = AlarmReason.NONE;
    running: RunningReason = RunningReason.JOB;
    axesHomed = 0;
    axesEnabled = 0;
    pos: [number, number, number, number] = [0, 0, 0, 0];

    /** Diagnostic: how many reply frames the sim has fed into the demux. */
    framesReplied = 0;

    private demux: Demux | null = null;
    private expectedSeq = 0; // mirrors the firmware's expectedSeq
    private pendingAcks = 0; // accepted but not yet flushed
    private aborting = false; // abort barrier (see MAGIC_ABORT)

    private motion: Array<{ ms: MicroSegment; interval: number; flags: number }> = [];
    private executing = false;
    private timeCredit = 0; // banked sim-seconds not yet spent
    private returnState: MachineState = MachineState.IDLE;

    private timer: ReturnType<typeof setInterval> | null = null;

    constructor(opts: SimOptions = {}) {
        this.ringSize = opts.ringSize ?? 64;
        this.ackCoalesceMax = opts.ackCoalesceMax ?? 8;
        this.frameMs = opts.frameMs ?? 40;
        this.fCpu = opts.fCpu ?? 150_000_000;
        this.timer = setInterval(() => this._tick(), this.frameMs);
        // Don't hold the event loop open — a test's vitest worker should exit
        // when the test completes. The timer is unref'd where supported.
        if (typeof this.timer === "object" && "unref" in this.timer) {
            (this.timer as { unref: () => void }).unref();
        }
    }

    /** Attach the demux the Sim feeds replies into (Link calls this on open). */
    attach(demux: Demux): void {
        this.demux = demux;
    }

    private reply(data: Uint8Array): void {
        this.framesReplied++;
        this.demux?.feed(data);
    }

    // ── Transport surface ───────────────────────────────────────────────────────

    write(data: Uint8Array): Promise<void> {
        if (data.length === 0) return Promise.resolve();

        // The Writer may hand a BATCH of frames, not one packet, so split before
        // dispatching. Text lines and STATUS_REQ are always written alone.
        const first = data[0]!;
        if (first < 0x80 && this._isTextLine(data)) {
            const line = new TextDecoder().decode(data).trim();
            if (line) this.reply(new TextEncoder().encode(this._handle(line) + "\n"));
            return Promise.resolve();
        }

        if (data.length === 1 && first === MAGIC_ABORT) {
            // The sim has no step loop to ramp, so it models the OUTCOME:
            // motion ends, the ring is discarded, position is kept. The ramp
            // distance the real machine would still travel is not simulated.
            this.motion = [];
            this.executing = false;
            this.timeCredit = 0;
            this.aborting = true;
            if (this.state === MachineState.RUNNING || this.state === MachineState.PAUSED) {
                this.state = MachineState.IDLE;
            }
            this.running = RunningReason.JOB;
            this.aborting = false;
            return Promise.resolve();
        }

        if (data.length === 1 && first === MAGIC_SEQRESET) {
            this.expectedSeq = 0;
            this.pendingAcks = 0;
            this.reply(new Uint8Array([MAGIC_ACK, 0, 0]));
            return Promise.resolve();
        }

        if (data.length === 1 && first === MAGIC_STATUS_REQ) {
            this.reply(
                packStatusRsp({
                    state: this.state,
                    axesEnabled: this.axesEnabled,
                    axesHomed: this.axesHomed,
                    alarm: this.alarm,
                    running: this.running,
                    bufCount: this.motion.length,
                    pos: this.pos,
                    expectedSeq: this.expectedSeq,
                    queuedUs: this._queuedUs(),
                }),
            );
            return Promise.resolve();
        }

        // A batch of 26-byte packets — split and dispatch each.
        for (let off = 0; off + 26 <= data.length; off += 26) {
            this._writePacket(data.subarray(off, off + 26));
        }
        this._flushAck(); // end of batch == the firmware's drain-empty flush
        return Promise.resolve();
    }

    read(): AsyncIterable<Uint8Array> {
        // The Sim has no raw byte stream — replies are fed straight into the
        // demux via `reply()`. The Link's read loop pumps this, consumes nothing
        // (chunks arrive synchronously through `write`), and exits on close.
        // An arrow-captured `this` (no `this` alias) closes over the instance.
        const readLoop = async function* (sim: SimTransport): AsyncGenerator<Uint8Array> {
            while (!sim._closed) {
                await new Promise<void>((r) => {
                    sim._readWaiters.push(r);
                });
                // The Sim feeds replies directly into the demux via write();
                // there is no byte chunk to yield. Loop until close drains the
                // waiters and exits.
                yield new Uint8Array(0);
            }
        };
        return { [Symbol.asyncIterator]: () => readLoop(this) };
    }
    private _closed = false;
    private _readWaiters: Array<() => void> = [];

    async close(): Promise<void> {
        this._closed = true;
        if (this.timer) clearInterval(this.timer);
        this.timer = null;
        for (const w of this._readWaiters) w();
        this._readWaiters = [];
    }

    // ── coalesced ACKs (mirrors data_plane.cpp §4.1) ───────────────────────────
    // The ACK is cumulative: one frame confirms every packet accepted since the
    // last flush. Deferring them is what the firmware does; the sim does it too
    // so the host suites actually exercise the multi-packet advance rather than
    // only ever seeing +1 deltas.

    private _flushAck(): void {
        if (this.pendingAcks > 0) {
            this.pendingAcks = 0;
            this.reply(new Uint8Array([MAGIC_ACK, this.expectedSeq, 0]));
        }
    }

    private _markAck(): void {
        this.pendingAcks++;
        if (this.pendingAcks >= this.ackCoalesceMax) {
            this.pendingAcks = 0;
            this.reply(new Uint8Array([MAGIC_ACK, this.expectedSeq, 0]));
        }
    }

    private _writePacket(data: Uint8Array): void {
        let ms: MicroSegment;
        try {
            ms = unpackMicrosegment(data);
        } catch {
            return; // not a recognised packet — drop
        }

        // Mirror the firmware's seq duplicate guard (data_plane.cpp): a stale
        // retransmit after a go-back is ACKed but NOT executed. Without this the
        // sim would duplicate motion on every retry and silently disagree with
        // hardware about final position.
        if (data[22] !== this.expectedSeq) {
            this.pendingAcks = 0; // immediate: the host's resync signal
            this.reply(new Uint8Array([MAGIC_ACK, this.expectedSeq, 0]));
            return;
        }
        if (this.state === MachineState.ALARM || this.state === MachineState.HOMING) {
            this._flushAck(); // ACKs earned before a rewind land first
            this.reply(new Uint8Array([MAGIC_NACK, NACK_BAD_STATE, 0]));
            return; // stream not accepted in these states
        }
        if (this.motion.length >= this.ringSize) {
            this._flushAck();
            this.reply(new Uint8Array([MAGIC_NACK, NACK_FULL, 0]));
            return; // backpressure — sender retries
        }
        if (!this.executing) {
            // Start a burst. From IDLE → returns to IDLE (a job). From PAUSED →
            // a jog during pause, returns to PAUSED. From RUNNING → the resumed
            // continuation after a tool-change PAUSE; returns to IDLE.
            this.returnState =
                this.state === MachineState.PAUSED ? MachineState.PAUSED : MachineState.IDLE;
            this.running = this.state === MachineState.PAUSED ? RunningReason.JOG : RunningReason.JOB;
            this.state = MachineState.RUNNING;
            this.executing = true;
        }
        this.motion.push({ ms, interval: ms.interval, flags: ms.flags });
        this.expectedSeq = (this.expectedSeq + 1) & 0xff;
        this._markAck();
    }

    /** Queued motion time, microseconds — the sim's mirror of queuedUs (§4.6). */
    private _queuedUs(): number {
        let total = 0;
        for (const { ms, interval } of this.motion) {
            const steps = Math.max(Math.abs(ms.dx), Math.abs(ms.dy), Math.abs(ms.dz), Math.abs(ms.da), 1);
            total += (interval * steps) / this.fCpu;
        }
        return Math.trunc(total * 1_000_000);
    }

    // ── the executor ───────────────────────────────────────────────────────────
    // Paced by each packet's own `interval` (CPU cycles/step) converted to
    // seconds via F_CPU, not by packet count — a packet's real duration is
    // `interval * steps / F_CPU`, so a 10mm@10mm/s jog takes ~1s here just
    // like on real hardware, regardless of how many packets the planner split
    // it into. `_time_credit` banks unspent wall-clock time across ticks so a
    // packet whose duration exceeds one FRAME_S tick still gets applied
    // atomically (position updates can't be split mid-packet) once enough
    // ticks have accrued to cover it.

    private _tick(): void {
        const frameS = this.frameMs / 1000;
        if (!this.executing || this.state !== MachineState.RUNNING) {
            this.timeCredit = 0;
            return; // idle, or paused/alarmed — hold
        }
        if (this.motion.length === 0) {
            this.executing = false; // burst complete
            this.state = this.returnState;
            this.running = RunningReason.JOB;
            this.timeCredit = 0;
            return;
        }
        this.timeCredit += frameS;
        while (this.motion.length > 0) {
            const { ms, interval, flags } = this.motion[0]!;
            const steps = Math.max(Math.abs(ms.dx), Math.abs(ms.dy), Math.abs(ms.dz), Math.abs(ms.da), 1);
            const duration = (interval * steps) / this.fCpu;
            if (duration > this.timeCredit) break; // not enough banked time yet
            this.motion.shift();
            this.timeCredit -= duration;
            this.pos[0] += ms.dx;
            this.pos[1] += ms.dy;
            this.pos[2] += ms.dz;
            this.pos[3] += ms.da;
            if (flags & MSEG_FLAG_PAUSE) {
                // MSEG_FLAG_PAUSE — predetermined stop; resume + next stream starts anew
                this.state = MachineState.PAUSED;
                this.executing = false;
                this.timeCredit = 0;
                break;
            }
        }
    }

    // ── control plane handler (called for a parsed text line) ───────────────────
    private _handle(line: string): string {
        const parts = line.split(/\s+/);
        const cmd = parts[0]!;
        const args = parts.slice(1);
        // `S` mirrors the Python `S = self`. eslint's no-this-alias rule wants
        // an arrow rather than a `this` alias; the handler has no nested
        // callbacks that would lose `this`, so the alias is stylistic parity
        // with the Python, not a binding hazard.
        // eslint-disable-next-line @typescript-eslint/no-this-alias
        const S = this;
        const idlePausedAlarm: readonly MachineState[] = [
    MachineState.IDLE,
    MachineState.PAUSED,
    MachineState.ALARM,
];
const isIdlePausedAlarm = (s: MachineState): boolean => idlePausedAlarm.indexOf(s) >= 0;

        switch (cmd) {
            case "ping":
                return "pong";
            case "seqreset":
                S.expectedSeq = 0;
                S.pendingAcks = 0; // a deferred ACK names the old numbering
                return "seq reset";
            case "pingnode": {
                // `all` (or no arg) answers on ONE line, not one per node — text
                // plane is one line per command.
                if (args.length === 0 || args[0] === "all") {
                    return "nodes " + [1, 2, 3, 4].map((n) => `${n}=ok`).join(" ");
                }
                return `node ${args[0]} ok`;
            }
            case "getstate":
                return (
                    `state=${S.state} enabled=0x${S.axesEnabled.toString(16).padStart(2, "0")} ` +
                    `homed=0x${S.axesHomed.toString(16).padStart(2, "0")} ` +
                    `alarm=${S.alarm} running=${S.running}`
                );
            case "getpos":
                return "pos " + S.pos.join(" ");
            case "stop": // always available; de-energises
                S.state = MachineState.ALARM;
                S.alarm = AlarmReason.ESTOP;
                S.axesHomed = 0;
                S.axesEnabled = 0;
                S.motion = [];
                S.executing = false;
                return "ok";
            case "enable":
                if (isIdlePausedAlarm(S.state)) {
                    if (args.length === 0 || args[0] === "all") {
                        S.axesEnabled = axisMask("xyza"); // energise all present axes
                    } else {
                        const node = parseInt(args[0]!, 10);
                        S.axesEnabled |= 1 << (node - 1);
                    }
                    return "ok";
                }
                return "err bad_state";
            case "disable":
                if (isIdlePausedAlarm(S.state)) {
                    if (args.length === 0 || args[0] === "all") {
                        S.axesHomed = 0; // de-energise -> position invalid
                        S.axesEnabled = 0;
                    } else {
                        const node = parseInt(args[0]!, 10);
                        S.axesEnabled &= ~(1 << (node - 1));
                        S.axesHomed &= ~(1 << (node - 1));
                    }
                    return "ok";
                }
                return "err bad_state";
            case "setorigin": {
                if (!isIdlePausedAlarm(S.state)) return "err bad_state";
                const axes = args[0] ?? "xyza";
                S.axesHomed |= axisMask(axes);
                const axisChars = "xyza";
                for (let i = 0; i < 4; i++) {
                    if (axes.includes(axisChars[i]!)) S.pos[i] = 0;
                }
                if (S.state === MachineState.ALARM) {
                    // setorigin recovers from ALARM
                    S.state = MachineState.IDLE;
                    S.alarm = AlarmReason.NONE;
                }
                return "ok";
            }
            case "pause":
                if (S.state === MachineState.RUNNING) {
                    S.state = MachineState.PAUSED; // executor holds; motion retained
                    return "ok";
                }
                return "err bad_state";
            case "resume":
                if (S.state === MachineState.PAUSED) {
                    // Phase 1: host pre-positions before resuming; Pico returns to
                    // IDLE and accepts a fresh stream for the next operation.
                    S.state = MachineState.IDLE;
                    S.motion = [];
                    S.executing = false;
                    return "ok";
                }
                return "err bad_state";
            case "cancel":
                if (S.state === MachineState.PAUSED) {
                    S.state = MachineState.IDLE;
                    S.motion = [];
                    S.executing = false;
                    return "ok";
                }
                return "err bad_state";
            case "unalarm":
                if (S.state === MachineState.ALARM) {
                    S.state = MachineState.IDLE;
                    S.alarm = AlarmReason.NONE;
                    return "ok";
                }
                return "err bad_state";
            default:
                return "err unknown";
        }
    }

    /** Test hook: detect a text write (all bytes < 0x80, ASCII-printable). */
    private _isTextLine(data: Uint8Array): boolean {
        for (let i = 0; i < data.length; i++) {
            const b = data[i]!;
            if (b >= 0x80) return false;
        }
        return true;
    }

    /** Test hook: drive the sim into RUNNING so pause/resume can be exercised. */
    _forceRunning(): void {
        this.state = MachineState.RUNNING;
    }
}