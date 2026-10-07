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
    MAGIC_JOG,
    MAGIC_BEZIER,
    MAGIC_MICROSEG,
    BEZIER_SIZE,
    BEZIER_SEQ_OFFSET,
    PACKET_SIZE,
    NACK_FULL,
    NACK_BAD_STATE,
    NACK_PAUSED,
    NACK_SOFT_LIMIT,
    MAGIC_CJOG,
    MAGIC_CJOG_STOP,
    CJOG_SIZE,
    CJOG_SPEED_ONE,
    CJOG_DEADMAN_MS,
} from "../../format/constants.js";
import {
    MachineState,
    AlarmReason,
    RunningReason,
    axisMask,
    packStatusRsp,
} from "../../format/status.js";
import { unpackMicrosegment } from "../../format/packet.js";
import { crc8 } from "../../format/crc.js";
import type { MicroSegment } from "../../format/microsegment.js";

const MSEG_FLAG_PAUSE = 0x04; // sender-inserted at a tool-change boundary

interface SimOptions {
    ringSize?: number;
    ackCoalesceMax?: number;
    frameMs?: number;
    fCpu?: number;
    /**
     * Bus ids that answer a relay. An id outside this set relays and times out,
     * exactly like a missing node on real RS485 — which is what makes
     * `axis_map` able to fail partway (`err node <id> timeout`).
     */
    busNodes?: readonly number[];
    /**
     * Commit this map at boot, as the firmware's controller does with the
     * config's defaultHead map. Without it the sim boots IDLE and unmapped, as
     * the firmware does with no config (core0.cpp).
     */
    axisMap?: readonly (number | null)[];
    /** Continuous-jog rate at 1× speed, steps/s on each moving axis. */
    cjogStepsPerS?: number;
    /**
     * X and Y travel, [lo, hi] steps, for a continuous jog to stop at. Without
     * it a jog runs until released.
     */
    cjogTravel?: readonly [readonly [number, number], readonly [number, number]];
    /** Allow a continuous jog on an unhomed axis (the config's jogUnhomed). */
    jogUnhomed?: boolean;
}

/** `get jogging=` values (shared_state.h JoggingReason). */
const JOGGING_CONT = 2;

/** Provisional bus-address ceiling — control_plane.cpp BUS_ADDR_MAX. */
const BUS_ADDR_MAX = 8;
/** Stream-byte motion slots (X/Y/Z/A) — control_plane.cpp MOTION_SLOTS. */
const MOTION_SLOTS = 4;
/** The `get` keys the Sim models (Sim._getKey). */
const SIM_GET_KEYS = [
    "state", "enabled", "homed", "alarm", "running", "latched",
    "probed", "pz", "nodehomed", "pos", "jogging",
] as const;

export class SimTransport implements Transport {
    readonly ringSize: number;
    readonly ackCoalesceMax: number;
    readonly frameMs: number;
    readonly fCpu: number;

    /** Ids that answer a relay (everything else times out). */
    readonly busNodes: ReadonlySet<number>;

    /** Unmapped is not an alarm: an empty map boots IDLE (core0.cpp). */
    state: MachineState = MachineState.IDLE;
    alarm: AlarmReason = AlarmReason.NONE;
    running: RunningReason = RunningReason.JOB;
    /**
     * DERIVED, per slot. Never assign it directly — call `_rederiveHomed()`.
     * The firmware holds the datum against the NODE (`nodeOrigin`/`nodeHomed`
     * in core0/position.cpp) and rebuilds this mask from the incoming node on
     * every bind, which is what lets a head that was homed earlier come back
     * homed after a re-bind instead of needing to re-home.
     *
     * The sim used to hold it per slot and leave it untouched across a rebind,
     * so `axis_map 1 2 3 4` → setorigin → `axis_map 1 2 5 6` reported head 1's
     * never-datumed Z as homed. Backwards from the machine, and on the exact
     * property the axis map exists to get right.
     */
    axesHomed = 0;
    axesEnabled = 0;

    /** Bus ids holding a valid datum — the truth `axesHomed` is derived from. */
    nodeHomed = new Set<number>();

    /**
     * Stored probe contact height per bus id (`setprobe`). Reported only while
     * the node is also in `nodeHomed`, which models every datum loss clearing it.
     */
    nodeProbe = new Map<number, number>();

    /**
     * Bus ids standing on their limit switch, and the per-slot view of it.
     * Node-framed for the same reason the datum is: a switch belongs to a
     * motor, not to a stream slot (core0/position.cpp).
     */
    nodeLatched = new Set<number>();
    axesLatched = 0;

    /**
     * The home in flight, if any. A crude model of the node-run sequence: the
     * node decides seek-vs-retract from ONE read of its own switch at arm time
     * and does not report which (docs/homing.md §1.2), so that read is all this
     * needs to reproduce the behaviour the host sequencer keys off.
     *
     * Deliberately not a step-by-step simulation. What the host has to get
     * right is the arm/poll/verdict protocol and the alternation of terminal
     * states, and this reproduces those exactly; how long the axis takes to
     * arrive is not something the host reasons about.
     */
    homing: { node: number; retract: boolean; park: boolean; until: number } | null = null;

    /** Wall-clock ms a modelled home leg takes. Short — it is not the point. */
    homingLegMs = 60;
    pos: [number, number, number, number] = [0, 0, 0, 0];

    /** Diagnostic: how many reply frames the sim has fed into the demux. */
    framesReplied = 0;

    private demux: Demux | null = null;
    private expectedSeq = 0; // mirrors the firmware's expectedSeq
    private pendingAcks = 0; // accepted but not yet flushed
    private aborting = false; // abort barrier (see MAGIC_ABORT)

    /**
     * The committed axis map: slotNode[i] is the bus id bound to stream slot i,
     * or null for unbound (control_plane.cpp `slotNode[]`, SLOT_NONE = 0xFF).
     * Core-0-local on the firmware, so it is NOT in STATUS_RSP — the host reads
     * it back with the no-arg `axis_map` (§8).
     */
    slotNode: (number | null)[] = [null, null, null, null];

    /**
     * Peripheral state, keyed by bus id (servos by `node:idx`). Held so a demo
     * or test can assert what the machine was actually told — the firmware's
     * relays are fire-and-check-ACK and report nothing back.
     */
    knifeOsc = new Map<number, boolean>();
    knifeBlower = new Map<number, number>();
    vacPump = new Map<number, boolean>();
    vacServo = new Map<string, boolean>();

    private motion: Array<{ ms: MicroSegment; interval: number; flags: number }> = [];
    private executing = false;
    private timeCredit = 0; // banked sim-seconds not yet spent
    private returnState: MachineState = MachineState.IDLE;

    private timer: ReturnType<typeof setInterval> | null = null;

    // Continuous jog (data_plane.cpp acceptCjog). The sim models no braking:
    // a stop or a new direction takes effect at once.
    readonly cjogStepsPerS: number;
    readonly cjogTravel: SimOptions["cjogTravel"];
    readonly jogUnhomed: boolean;
    private cjogDir: [number, number] = [0, 0];
    private cjogSpeed = CJOG_SPEED_ONE;
    private cjogHeld = false; // packets keep arriving for cjogDir
    private cjogMoving = false; // not yet at the end of travel
    private cjogLastMs = 0;
    private cjogFrac: [number, number] = [0, 0]; // sub-step progress

    constructor(opts: SimOptions = {}) {
        this.ringSize = opts.ringSize ?? 64;
        this.ackCoalesceMax = opts.ackCoalesceMax ?? 8;
        this.frameMs = opts.frameMs ?? 40;
        this.fCpu = opts.fCpu ?? 150_000_000;
        this.busNodes = new Set(opts.busNodes ?? [1, 2, 3, 4]);
        this.cjogStepsPerS = opts.cjogStepsPerS ?? 2000;
        this.cjogTravel = opts.cjogTravel;
        this.jogUnhomed = opts.jogUnhomed ?? false;
        if (opts.axisMap) {
            for (let i = 0; i < MOTION_SLOTS; i++) this.slotNode[i] = opts.axisMap[i] ?? null;
            this.state = MachineState.IDLE;
            this.alarm = AlarmReason.NONE;
        }
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

        if (data.length === 1 && first === MAGIC_CJOG_STOP) {
            this._cjogStop();
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

        // A batch of packets — split by each one's magic and dispatch.
        for (let off = 0; off < data.length; ) {
            const m = data[off];
            const size = m === MAGIC_BEZIER ? BEZIER_SIZE
                : m === MAGIC_MICROSEG || m === MAGIC_JOG ? PACKET_SIZE
                : m === MAGIC_CJOG ? CJOG_SIZE
                : 0;
            if (size === 0 || off + size > data.length) break; // unframeable — drop the rest
            const pkt = data.subarray(off, off + size);
            if (m === MAGIC_BEZIER) this._writeBezier(pkt);
            else if (m === MAGIC_CJOG) this._writeCjog(pkt);
            else this._writePacket(pkt);
            off += size;
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

    // ── continuous jog (mirrors data_plane.cpp acceptCjog / cjogTick) ──────────

    private _writeCjog(data: Uint8Array): void {
        if (crc8(data, 0, CJOG_SIZE - 1) !== data[CJOG_SIZE - 1]) return; // corrupt — drop
        const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const d = [dv.getInt8(1), dv.getInt8(2), dv.getInt8(3), dv.getInt8(4)];
        const speed = data[5]!;
        const bad = d.some((v, k) => v < -1 || v > 1 || (k >= 2 && v !== 0));
        if (bad || speed === 0) return this._nack(NACK_BAD_STATE);

        this.cjogLastMs = Date.now();
        if (d[0] === 0 && d[1] === 0) return this._cjogStop();
        if (this.cjogHeld && d[0] === this.cjogDir[0] && d[1] === this.cjogDir[1]) return; // a repeat

        const jogging = this.state === MachineState.JOGGING;
        if (this.state !== MachineState.IDLE && !jogging) return this._nack(NACK_BAD_STATE);
        for (let k = 0; k < 2; k++) {
            if (d[k] !== 0 && !(this.axesHomed & (1 << k)) && !this.jogUnhomed) {
                return this._nack(NACK_BAD_STATE);
            }
        }
        const dir: [number, number] = [d[0]!, d[1]!];
        if (!this._cjogRoom(dir)) {
            this._cjogStop();
            return this._nack(NACK_SOFT_LIMIT);
        }
        this.cjogDir = dir;
        this.cjogSpeed = speed;
        this.cjogHeld = this.cjogMoving = true;
        this.cjogFrac = [0, 0];
        this.state = MachineState.JOGGING;
        this.reply(new Uint8Array([MAGIC_ACK, this.expectedSeq, 0]));
    }

    private _nack(reason: number): void {
        this.reply(new Uint8Array([MAGIC_NACK, reason, 0]));
    }

    /** Some travel left on every moving axis. */
    private _cjogRoom(dir: readonly [number, number]): boolean {
        if (!this.cjogTravel) return true;
        return dir.every((v, k) => {
            const [lo, hi] = this.cjogTravel![k]!;
            return v === 0 || (v > 0 ? this.pos[k]! < hi : this.pos[k]! > lo);
        });
    }

    private _cjogStop(): void {
        this.cjogHeld = this.cjogMoving = false;
        if (this.state === MachineState.JOGGING) this.state = MachineState.IDLE;
    }

    private _tickCjog(frameS: number): void {
        if (this.cjogHeld && Date.now() - this.cjogLastMs > CJOG_DEADMAN_MS) {
            this._cjogStop();
            return;
        }
        if (!this.cjogMoving) return;
        const run = (this.cjogStepsPerS * this.cjogSpeed) / CJOG_SPEED_ONE * frameS;
        let ended = false;
        for (let k = 0; k < 2; k++) {
            const v = this.cjogDir[k]!;
            if (v === 0) continue;
            this.cjogFrac[k]! += run;
            const n = Math.floor(this.cjogFrac[k]!);
            this.cjogFrac[k]! -= n;
            let p = this.pos[k]! + v * n;
            if (this.cjogTravel) {
                const [lo, hi] = this.cjogTravel[k]!;
                if (p <= lo || p >= hi) { p = Math.min(hi, Math.max(lo, p)); ended = true; }
            }
            this.pos[k] = p;
        }
        // At the end of travel the jog stops but stays held: repeats stay silent.
        if (ended) {
            this.cjogMoving = false;
            this.state = MachineState.IDLE;
        }
    }

    /** BEZIER records are framed, CRC- and seq-checked, and ACKed; the sim does not move. */
    private _writeBezier(data: Uint8Array): void {
        if (crc8(data, 0, BEZIER_SIZE - 1) !== data[BEZIER_SIZE - 1]) return; // corrupt — drop
        if (data[BEZIER_SEQ_OFFSET] !== this.expectedSeq) {
            this.pendingAcks = 0; // immediate: the host's resync signal
            this.reply(new Uint8Array([MAGIC_ACK, this.expectedSeq, 0]));
            return;
        }
        this.expectedSeq = (this.expectedSeq + 1) & 0xff;
        this._markAck();
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
        // State gate, per magic (data_plane.cpp). The two stream types differ:
        //   MSEG (job) — IDLE/RUNNING only; PAUSED gets its own NACK_PAUSED so
        //     the host can hold rather than treat it as an error.
        //   JOG        — IDLE/PAUSED, plus RUNNING when the burst in progress is
        //     itself a jog. Packet 2+ of a multi-packet jog arrives after the
        //     machine already flipped to RUNNING for packet 1; rejecting those
        //     would NACK every jog after the first, forever.
        // ALARM lands here too, which is how every alarm refuses all motion
        // without a separate predicate.
        const isJog = data[0] === MAGIC_JOG;
        const st = this.state;
        if (!isJog) {
            if (st === MachineState.PAUSED) {
                this._flushAck();
                this.reply(new Uint8Array([MAGIC_NACK, NACK_PAUSED, 0]));
                return;
            }
            if (st !== MachineState.IDLE && st !== MachineState.RUNNING) {
                this._flushAck(); // ACKs earned before a rewind land first
                this.reply(new Uint8Array([MAGIC_NACK, NACK_BAD_STATE, 0]));
                return;
            }
        } else {
            const continuingJog = st === MachineState.RUNNING && this.running === RunningReason.JOG;
            if (st !== MachineState.IDLE && st !== MachineState.PAUSED && !continuingJog) {
                this._flushAck();
                this.reply(new Uint8Array([MAGIC_NACK, NACK_BAD_STATE, 0]));
                return;
            }
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
            // runningReason follows the STREAM TYPE, not the entry state: a jog
            // from IDLE is RUNNING_JOG, and that is exactly what lets packet 2+
            // of the burst past the gate above.
            this.running = isJog ? RunningReason.JOG : RunningReason.JOB;
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
        this._tickHoming();
        const frameS = this.frameMs / 1000;
        this._tickCjog(frameS);
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
                // plane is one line per command. The scan is the whole bus
                // (1..BUS_ADDR_MAX), not just the axes: it is the bring-up verb
                // that surfaces peripherals too.
                if (!isIdlePausedAlarm(S.state)) return "err bad_state";
                if (args.length === 0 || args[0] === "all") {
                    let out = "nodes";
                    for (let n = 1; n <= BUS_ADDR_MAX; n++) {
                        out += ` ${n}=${S.busNodes.has(n) ? "ok" : "timeout"}`;
                    }
                    return out;
                }
                const node = parseInt(args[0]!, 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err bad_node";
                return `node ${node} ${S.busNodes.has(node) ? "ok" : "timeout"}`;
            }
            case "nodepos": {
                // The node's OWN counter. The sim has no per-node counter
                // distinct from machinePos, so it reports the slot's position —
                // which is the no-lost-steps case, the only one a sim can model.
                if (!isIdlePausedAlarm(S.state)) return "err bad_state";
                const node = parseInt(args[0] ?? "", 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err usage";
                if (!S.busNodes.has(node)) return `node ${node} timeout`;
                const slot = S._nodeSlot(node);
                return `node ${node} pos ${slot === null ? 0 : S.pos[slot]}`;
            }
            case "axis_map": {
                // Read-back form (§8): the map is host-authored and absent from
                // STATUS_RSP, so this is the only way to see what is committed.
                if (args.length === 0) {
                    return "axis_map " + S.slotNode.map((n) => (n === null ? "-" : String(n))).join(" ");
                }
                // Rebinding mid-RUNNING would corrupt in-flight motion (§6.2).
                if (!isIdlePausedAlarm(S.state)) return "err bad_state";

                const desired: (number | null)[] = [];
                for (let i = 0; i < MOTION_SLOTS; i++) {
                    const tok = args[i];
                    if (tok === undefined || tok === "") return "err usage";
                    if (tok === "-") { desired.push(null); continue; }
                    const v = parseInt(tok, 10);
                    if (Number.isNaN(v)) return "err usage";
                    if (v === 0) { desired.push(null); continue; }
                    if (v > BUS_ADDR_MAX) return "err bad_node";
                    desired.push(v);
                }
                for (let i = 0; i < MOTION_SLOTS; i++) {
                    for (let j = i + 1; j < MOTION_SLOTS; j++) {
                        if (desired[i] !== null && desired[i] === desired[j]) return "err dup";
                    }
                }

                // Deliberately NOT a diff: disengage everything previously
                // bound, then engage every desired node unconditionally, so a
                // node that silently lost its slot is always re-bound. A node
                // that does not ACK leaves the map untouched — a retry redoes
                // all of it (idempotent, no rollback needed).
                for (const id of desired) {
                    if (id !== null && !S.busNodes.has(id)) return `err node ${id} timeout`;
                }
                S.slotNode = desired;
                // Each incoming node brings its own datum with it, and a slot
                // that lost its node loses the bit. This is the whole point of
                // holding the datum against the node: swapping heads does not
                // mean re-homing, and swapping BACK does not mean re-homing
                // either.
                S._rederiveHomed();
                // Same for the latch: an incoming node standing on its switch
                // brings that with it, and gates the machine again.
                S._rederiveLatched();
                return "ok";
            }
            // `leg <node> seek|retract <dir> <startUs> <floorUs> <rampSteps> <maxSteps>`
            // `leg <node> park <target> <startUs> <floorUs> <rampSteps>`
            // `leg <node> sweep …`
            // — arms ONE leg and returns; the machine sits in HOMING until
            // _tickHoming() finishes it. The numbers are accepted and ignored:
            // this models the protocol, not the motion. seek/retract is not
            // ignored — it is the intent the real node checks before arming
            // (include/common.h, CMD_HOME_LEG).
            //
            // Addresses a BUS ID, like the firmware. No `err unconfigured` and no
            // map lookup: a leg needs no committed axis_map, which is the point
            // of node addressing. Nothing here needs the slot either —
            // _tickHoming() writes the node-framed masks and re-derives the
            // per-slot views, so an unbound node models correctly with no
            // special case.
            case "leg": {
                if (S.homing !== null) return "err busy";
                if (!isIdlePausedAlarm(S.state)) return "err bad_state";
                const node = parseInt(args[0] ?? "", 10);
                if (!Number.isInteger(node)) return "err usage";
                const verb = args[1];
                // This sim models no index node: no Hall capture, no `index`,
                // no `steprev`. Answering the way a real Pico answers a
                // wrong-kind verb is honest; pretending to sweep would let a
                // test pass against a rotary path that was never exercised.
                if (verb === "sweep") return `err kind_mismatch node ${node} is 1 want 2`;
                if (verb === "park") {
                    if (!S.nodeHomed.has(node)) return "err not_homed";
                    // A park starts clear of its switch.
                    if (S.nodeLatched.has(node)) return `err node ${node} nak intent_mismatch`;
                    S.homing = { node, retract: false, park: true, until: Date.now() + S.homingLegMs };
                    S.state = MachineState.HOMING;
                    return "ok";
                }
                if (verb !== "seek" && verb !== "retract") return "err usage";
                // The node reads its own switch ONCE, here, and that read alone
                // decides seek vs retract. The verb is checked AGAINST it,
                // mirroring the real node's CMD_HOME_LEG handler.
                const retract = S.nodeLatched.has(node);
                if ((verb === "retract") !== retract) return `err node ${node} nak intent_mismatch`;
                S.homing = { node, retract, park: false, until: Date.now() + S.homingLegMs };
                S.state = MachineState.HOMING;
                return "ok";
            }
            // `leg_abort <node>`: stops the supervised leg as a failure; any
            // other node just acks.
            case "leg_abort": {
                const node = parseInt(args[0] ?? "", 10);
                if (!Number.isInteger(node)) return "err usage";
                if (S.homing !== null && S.homing.node === node) {
                    S.homing = null;
                    S.nodeHomed.delete(node);
                    S._rederiveHomed();
                    S.state = MachineState.ALARM;
                    S.alarm = AlarmReason.HOMING_FAIL;
                }
                return "ok";
            }
            // Keys the Sim does not model answer `!`, as older firmware would.
            case "get": {
                const keys = args.filter((k) => k !== "");
                if (keys.length === 0) return "keys " + SIM_GET_KEYS.join(" ");
                if (keys.length > 32) return "err too_many_keys";
                return keys.map((k) => `${k}=${S._getKey(k) ?? "!"}`).join(" ");
            }
            case "setprobe": {
                if (S.state !== MachineState.IDLE && S.state !== MachineState.PAUSED) {
                    return "err bad_state";
                }
                const z = parseInt(args[0] ?? "", 10);
                if (!Number.isFinite(z)) return "err usage";
                const n = S.slotNode[2];
                if (n === null || n === undefined) return "err unbound";
                if (!S.nodeHomed.has(n)) return "err not_homed";
                S.nodeProbe.set(n, z);
                return `ok probe node=${n} z=${z}`;
            }
            case "unprobe": {
                let n: number | null | undefined = S.slotNode[2];
                if (args[0] !== undefined) {
                    n = parseInt(args[0], 10);
                    if (!(n >= 1 && n <= BUS_ADDR_MAX)) return "err bad_node";
                } else if (n === null || n === undefined) {
                    return "err unbound";
                }
                S.nodeProbe.delete(n);
                return `ok unprobe node=${n}`;
            }
            case "stop": // always available; de-energises
                S.state = MachineState.ALARM;
                S.alarm = AlarmReason.ESTOP;
                // An estop de-energises everything, so every node loses its
                // datum — not just the four currently bound.
                S.nodeHomed.clear();
                S._rederiveHomed();
                S.homing = null;          // an estop abandons a home in flight
                S.axesEnabled = 0;
                S.motion = [];
                S.executing = false;
                return "ok";
            // enable/disable are TYPE-BLIND relays (§9): they go to any bus id,
            // and the axis bookkeeping applies only when that id is in the axis
            // map. The bit index is the id's SLOT, never `id - 1` — an axis node
            // can be any bus address now.
            // axes_enable targets the MAP — bound slots only, never peripherals.
            case "axes_enable":
                if (isIdlePausedAlarm(S.state)) {
                    if (args.length === 0) return "err usage";
                    // Nothing bound means nothing to enable (cmd/axis.cpp).
                    if (S.slotNode.every((n) => n === null)) return "err unbound";
                    const on = args[0] === "1" || args[0]!.toLowerCase() === "on";
                    for (let i = 0; i < MOTION_SLOTS; i++) {
                        if (S.slotNode[i] === null) continue;
                        if (on) S.axesEnabled |= 1 << i;
                        else {
                            S.axesEnabled &= ~(1 << i);
                            // De-energise -> datum lost, and lost for the NODE:
                            // the motor may have been back-driven while off, so
                            // re-binding it elsewhere must not resurrect it.
                            S.nodeHomed.delete(S.slotNode[i]!);
                        }
                    }
                    S._rederiveHomed();
                    return "ok";
                }
                return "err bad_state";
            case "enable":
                if (isIdlePausedAlarm(S.state)) {
                    const node = parseInt(args[0] ?? "", 10);
                    if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err bad_node";
                    const slot = S._nodeSlot(node);
                    if (slot !== null) S.axesEnabled |= 1 << slot;
                    return "ok";
                }
                return "err bad_state";
            case "disable":
                if (isIdlePausedAlarm(S.state)) {
                    const node = parseInt(args[0] ?? "", 10);
                    if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err bad_node";
                    const slot = S._nodeSlot(node);
                    if (slot !== null) S.axesEnabled &= ~(1 << slot);
                    // Per NODE, and unconditionally: de-energising a motor
                    // costs its datum whether or not it currently holds a slot.
                    S.nodeHomed.delete(node);
                    S._rederiveHomed();
                    return "ok";
                }
                return "err bad_state";
            case "setorigin": {
                if (!isIdlePausedAlarm(S.state)) return "err bad_state";
                const axes = args[0] ?? "xyza";
                const m = axisMask(axes);
                const axisChars = "xyza";
                for (let i = 0; i < 4; i++) {
                    if (!(m & (1 << i))) continue;
                    // Recorded against the NODE in the slot, not the slot — an
                    // unbound slot has nothing to datum and is skipped, which is
                    // also why the firmware answers `err unbound` when NOTHING
                    // in the mask resolved.
                    const n = S.slotNode[i];
                    if (n === null || n === undefined) continue;
                    S.nodeHomed.add(n);
                    S.nodeProbe.delete(n);
                    S.pos[i] = 0;
                }
                S._rederiveHomed();
                // setorigin recovers from an ESTOP-alarm.
                if (S.state === MachineState.ALARM) {
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
                    // A latched limit is not cleared by asking: `unalarm` moves
                    // nothing, so the condition still holds afterwards. Answers
                    // `ok` and stays in ALARM — a retract is the way out.
                    if (S.axesLatched !== 0) {
                        S.alarm = AlarmReason.LIMIT_LATCHED;
                        return "ok";
                    }
                    S.state = MachineState.IDLE;
                    S.alarm = AlarmReason.NONE;
                    return "ok";
                }
                return "err bad_state";
            // ── peripheral relays (knife / vacuum) ───────────────────────────
            // Deliberately NOT state-gated: the firmware's gates on these five
            // verbs are commented out so the operator can work the oscillator,
            // blower and vacuum DURING a cut (control_plane.cpp). A sim that
            // still refused them while RUNNING would make the demo look broken
            // in exactly the case the change was made for.
            //
            // Replies follow the relay convention — `node <id> ok`, not `ok` —
            // because the answer is about a bus node, not the Pico.
            case "knife_osc": {
                const node = parseInt(args[0] ?? "", 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX) || args[1] === undefined) return "err usage";
                if (!S.busNodes.has(node)) return `node ${node} timeout`;
                S.knifeOsc.set(node, args[1] === "on" || args[1] === "1");
                return `node ${node} ok`;
            }
            case "knife_blower": {
                const node = parseInt(args[0] ?? "", 10);
                const duty = parseInt(args[1] ?? "", 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX) || !(duty >= 0 && duty <= 100)) return "err usage";
                if (!S.busNodes.has(node)) return `node ${node} timeout`;
                S.knifeBlower.set(node, duty);
                return `node ${node} ok`;
            }
            case "vac_pump": {
                const node = parseInt(args[0] ?? "", 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX) || args[1] === undefined) return "err usage";
                if (!S.busNodes.has(node)) return `node ${node} timeout`;
                S.vacPump.set(node, args[1] === "on" || args[1] === "1");
                return `node ${node} ok`;
            }
            case "vac_servo": {
                const node = parseInt(args[0] ?? "", 10);
                const idx = parseInt(args[1] ?? "", 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX) || !(idx >= 0 && idx <= 6) || args[2] === undefined) {
                    return "err usage";
                }
                if (!S.busNodes.has(node)) return `node ${node} timeout`;
                S.vacServo.set(`${node}:${idx}`, args[2] === "on" || args[2] === "1");
                return `node ${node} ok`;
            }
            case "vac_switch": {
                const node = parseInt(args[0] ?? "", 10);
                if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err usage";
                if (!S.busNodes.has(node)) return `node ${node} timeout`;
                // Nothing actuates the switch in a sim — it reads at rest.
                return `node ${node} switch closed (level=0)`;
            }
            default:
                return "err unknown";
        }
    }

    /**
     * Finish a modelled home once its leg time is up.
     *
     * The verdict is decided by which move this was, exactly as on the Pico: a
     * seek ends ON the switch, a retract ends OFF it, and the terminal state
     * follows from the latch mask rather than from the leg — so an axis parked
     * clear lands IDLE while one still holding a switch lands
     * ALARM/LIMIT_LATCHED, whichever leg put it there.
     */
    private _tickHoming(): void {
        const h = this.homing;
        if (h === null || Date.now() < h.until) return;
        this.homing = null;

        // A park ends off its switch with the datum kept: it moved within it.
        if (!h.park) {
            if (h.retract) this.nodeLatched.delete(h.node);
            else this.nodeLatched.add(h.node);
            this._rederiveLatched();

            // A home moves the axis with the NODE's own pulser, which the
            // master does not count, so the datum is dropped either way —
            // §3.4's closing `setorigin` is what re-derives it.
            this.nodeHomed.delete(h.node);
            this._rederiveHomed();
        }

        if (this.axesLatched !== 0) {
            this.state = MachineState.ALARM;
            this.alarm = AlarmReason.LIMIT_LATCHED;
        } else {
            this.state = MachineState.IDLE;
            this.alarm = AlarmReason.NONE;
        }
    }

    /** Per-slot view of `nodeLatched`, rebuilt on every bind — as for the datum. */
    _rederiveLatched(): void {
        let m = 0;
        for (let s = 0; s < MOTION_SLOTS; s++) {
            const n = this.slotNode[s];
            if (n !== null && n !== undefined && this.nodeLatched.has(n)) m |= 1 << s;
        }
        this.axesLatched = m;
    }

    /**
     * Rebuild `axesHomed` from `nodeHomed` and the current bindings, the way
     * slotAdoptStatus() does on the Pico. Called after anything that changes
     * either — a rebind, a datum, a de-energise.
     */
    _rederiveHomed(): void {
        let m = 0;
        for (let s = 0; s < MOTION_SLOTS; s++) {
            const n = this.slotNode[s];
            if (n !== null && n !== undefined && this.nodeHomed.has(n)) m |= 1 << s;
        }
        this.axesHomed = m;
    }

    /** One `get` key's value, `-` when it does not apply, undefined if unmodelled. */
    _getKey(key: string): string | undefined {
        const hex = (v: number, w: number) => `0x${v.toString(16).padStart(w, "0")}`;
        switch (key) {
            case "state":     return String(this.state);
            case "enabled":   return hex(this.axesEnabled, 2);
            case "homed":     return hex(this.axesHomed, 2);
            case "alarm":     return String(this.alarm);
            case "running":   return String(this.running);
            case "latched":   return hex(this.axesLatched, 2);
            case "probed":    return this._probeZ() === undefined ? "0" : "1";
            case "pz":        return String(this._probeZ() ?? "-");
            case "nodehomed": {
                let m = 0;
                for (const n of this.nodeHomed) m |= 1 << n;
                return hex(m, 3);
            }
            case "pos":       return this.pos.join(",");
            case "jogging":   return this.state === MachineState.JOGGING ? String(JOGGING_CONT) : "-";
            default:          return undefined;
        }
    }

    /** The probe height held for the Z in slot 2, if any. */
    private _probeZ(): number | undefined {
        const n = this.slotNode[2];
        return n !== null && n !== undefined && this.nodeHomed.has(n)
            ? this.nodeProbe.get(n)
            : undefined;
    }

    /** Which stream slot a bus id is ENGAGE-bound to, or null (nodeSlot()). */
    private _nodeSlot(node: number): number | null {
        const i = this.slotNode.indexOf(node);
        return i < 0 ? null : i;
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