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
    NACK_FULL,
    NACK_BAD_STATE,
    NACK_PAUSED,
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
    /**
     * Bus ids that answer a relay. An id outside this set relays and times out,
     * exactly like a missing node on real RS485 — which is what makes
     * `axis_map` able to fail partway (`err node <id> timeout`).
     */
    busNodes?: readonly number[];
    /**
     * Skip the ALARM_CONFIG boot gate by committing this map immediately, as if
     * `axis_map` had already run. For tests that are not about the gate; real
     * firmware always boots unconfigured (core0.cpp).
     */
    axisMap?: readonly (number | null)[];
}

/** Provisional bus-address ceiling — control_plane.cpp BUS_ADDR_MAX. */
const BUS_ADDR_MAX = 8;
/** Stream-byte motion slots (X/Y/Z/A) — control_plane.cpp MOTION_SLOTS. */
const MOTION_SLOTS = 4;

export class SimTransport implements Transport {
    readonly ringSize: number;
    readonly ackCoalesceMax: number;
    readonly frameMs: number;
    readonly fCpu: number;

    /** Ids that answer a relay (everything else times out). */
    readonly busNodes: ReadonlySet<number>;

    /**
     * Boot state is the ALARM_CONFIG gate, not IDLE (core0.cpp): the axis map
     * is empty, and since motion ingest gates on machineState alone, every
     * job/jog is refused until `axis_map` commits a binding.
     */
    state: MachineState = MachineState.ALARM;
    alarm: AlarmReason = AlarmReason.CONFIG;
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

    constructor(opts: SimOptions = {}) {
        this.ringSize = opts.ringSize ?? 64;
        this.ackCoalesceMax = opts.ackCoalesceMax ?? 8;
        this.frameMs = opts.frameMs ?? 40;
        this.fCpu = opts.fCpu ?? 150_000_000;
        this.busNodes = new Set(opts.busNodes ?? [1, 2, 3, 4]);
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
        // State gate, per magic (data_plane.cpp). The two stream types differ:
        //   MSEG (job) — IDLE/RUNNING only; PAUSED gets its own NACK_PAUSED so
        //     the host can hold rather than treat it as an error.
        //   JOG        — IDLE/PAUSED, plus RUNNING when the burst in progress is
        //     itself a jog. Packet 2+ of a multi-packet jog arrives after the
        //     machine already flipped to RUNNING for packet 1; rejecting those
        //     would NACK every jog after the first, forever.
        // ALARM lands here too, which is how the ALARM_CONFIG boot gate refuses
        // all motion without a separate predicate.
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

                // The gate condition is "every slot ACK-confirmed", not "a
                // string parsed" — reaching here means it held.
                if (S.state === MachineState.ALARM && S.alarm === AlarmReason.CONFIG) {
                    S.state = MachineState.IDLE;
                    S.alarm = AlarmReason.NONE;
                }
                return "ok";
            }
            case "getstate":
                return (
                    `state=${S.state} enabled=0x${S.axesEnabled.toString(16).padStart(2, "0")} ` +
                    `homed=0x${S.axesHomed.toString(16).padStart(2, "0")} ` +
                    `alarm=${S.alarm} running=${S.running}`
                );
            case "getpos":
                // Trailing validity mask, as the firmware does — the counts are
                // always plain numbers, never a sentinel.
                return (
                    "pos " + S.pos.join(" ") +
                    ` homed=0x${S.axesHomed.toString(16).padStart(2, "0")}`
                );
            case "stop": // always available; de-energises
                S.state = MachineState.ALARM;
                S.alarm = AlarmReason.ESTOP;
                S.axesHomed = 0;
                S.axesEnabled = 0;
                S.motion = [];
                S.executing = false;
                return "ok";
            // enable/disable are TYPE-BLIND relays (§9): they go to any bus id,
            // and the axis bookkeeping applies only when that id is in the axis
            // map. The bit index is the id's SLOT, never `id - 1` — an axis node
            // can be any bus address now.
            case "enable":
                if (isIdlePausedAlarm(S.state)) {
                    if (args.length === 0 || args[0] === "all") {
                        // `all` targets the map — bound slots only.
                        for (let i = 0; i < MOTION_SLOTS; i++) {
                            if (S.slotNode[i] !== null) S.axesEnabled |= 1 << i;
                        }
                    } else {
                        const node = parseInt(args[0]!, 10);
                        if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err bad_node";
                        const slot = S._nodeSlot(node);
                        if (slot !== null) S.axesEnabled |= 1 << slot;
                    }
                    return "ok";
                }
                return "err bad_state";
            case "disable":
                if (isIdlePausedAlarm(S.state)) {
                    if (args.length === 0 || args[0] === "all") {
                        S.axesHomed = 0; // de-energise -> datum lost
                        S.axesEnabled = 0;
                    } else {
                        const node = parseInt(args[0]!, 10);
                        if (!(node >= 1 && node <= BUS_ADDR_MAX)) return "err bad_node";
                        const slot = S._nodeSlot(node);
                        if (slot !== null) {
                            S.axesEnabled &= ~(1 << slot);
                            S.axesHomed &= ~(1 << slot);
                        }
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
                // setorigin recovers from an ESTOP-alarm, but NOT from the
                // config gate — only a committed axis_map leaves that (§6.1).
                if (S.state === MachineState.ALARM && S.alarm !== AlarmReason.CONFIG) {
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
                    // The config gate is not a clearable fault (§6.1).
                    if (S.alarm === AlarmReason.CONFIG) return "err unconfigured";
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