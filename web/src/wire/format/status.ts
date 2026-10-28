/**
 * format/status.ts — binary STATUS_RSP (0xA3, 37B) + the operational enums.
 * MachineState / Fault / JobReason, MachineStatus, parseStatusRsp,
 * packStatusRsp, axisMask.
 *
 * STATUS_RSP is one coherent sample in the node frame: state, the node
 * masks, the selection, each head's tool, WPos, the ring and the job's executing
 * record. The text `get` query carries the rest; statusFromGet builds the
 * same MachineStatus.
 *
 * A field a plane cannot carry stays `undefined` on a MachineStatus from that
 * plane, distinguishing "no information" from a real zero.
 */

import { crc8 } from "./crc.js";
import {
    MAGIC_STATUS_RSP,
    MAGIC_STATUS_RSP_V1,
    MAGIC_STATUS_RSP_V2,
    MAGIC_STATUS_RSP_V3,
    STATUS_HEADS,
    STATUS_RSP_SIZE,
} from "./constants.js";

// ── operational enums (mirror firmware values; docs/wire_protocol.md) ──────────

export const MachineState = {
    IDLE: 0,
    // 1 (RUNNING) and 4 (PAUSED) are retired.
    ESTOP: 2, // transient inter-core flush signal; rarely seen by the host
    ALARM: 3,
    HOMING: 5,
    /**
     * A tool-height probe session is open (docs/tool_probe.md §5.1). Entered by
     * `probe_map`, spans several legs, and left only by an explicit exit.
     *
     * Load-bearing for the same reason LIMIT_LATCHED is below: enumFromInt()
     * coerces an unrecognised value to the fallback, so a host missing this
     * entry renders a probing machine as IDLE — and would happily start a job
     * against a machine whose axis map is currently a probe binding.
     */
    PROBING: 6,
    /**
     * Planner jog motion (`line`, `bez`, `jog`), and `jogto`, which parks the
     * Zs before its line (reason 3, GOTO, throughout). Never pauses.
     */
    JOGGING: 7,
    /**
     * Node legs that keep the origin (`leg park|span|unlatch`) outside a homing
     * session. Opened by the first leg, closed by the Pico when the legs end.
     * Load-bearing, as PROBING above.
     */
    TRAVERSE: 8,
    /** A job on the planner (BEZIER records); JobReason says what it is doing. */
    JOB: 9,
} as const;
export type MachineState = (typeof MachineState)[keyof typeof MachineState];
const MACHINE_STATE_VALUES = Object.values(MachineState) as readonly number[];

/**
 * The fault mask, `faults` (MachineFault in core0/ops/machine.h). Several bits
 * may be set at once. Held faults clear by themselves when their cause goes;
 * event faults stay until `unalarm`. ESTOP is the estop latch, not a fault,
 * cleared by `unstop`. ALARM at rest means the mask is non-zero.
 */
export const Fault = {
    /** Held: the boot sweep left a node mute that is not excluded. */
    BUS: 0x0001,
    /** Held: no usable config (get `cfgerr=` says why). */
    CONFIG: 0x0002,
    /** Held: the slot request is unmet (a node did not engage). */
    UNMAPPED: 0x0004,
    /** Held: an axis stands on its limit switch; the node refuses stream steps. */
    LATCHED: 0x0008,
    /** Event: a homing or traverse leg failed (`homefail=` says how). */
    HOMING: 0x0010,
    /** Event: a probe leg failed (`probe=` says how). */
    PROBE: 0x0020,
    /** Event: the Pico dropped an internal fact; a firmware bug. */
    FACTS: 0x0040,
    /** Event: a sequence was refused after its ok (`seqfail=` says why). */
    SEQUENCE: 0x0080,
    /** Event: a tool's power switch failed its retries (a tool event). */
    TOOL: 0x0100,
    /** The estop latch. */
    ESTOP: 0x8000,
} as const;
export type Fault = (typeof Fault)[keyof typeof Fault];

/** What a job is doing, while JOB (JobReason in ipc/shared_state.h). */
export const JobReason = {
    /** At rest, waiting for enough queued motion. */
    BUFFERING: 0,
    RUNNING: 1,
    /** Braking for a pause or a late block. */
    HOLDING: 2,
    /** At rest with the ring dropped; a matching LAYER_HDR or `cancel`. */
    PAUSED: 3,
    /** Braking for an abort; ends IDLE. */
    ABORTING: 4,
    /** A tool change moving: the Zs park, the head goes to its probe switch. */
    TOOL_CHANGE: 5,
    /** At rest for the operator: mount the tool, jog, zero Z. */
    TOOL_WAIT: 6,
    /** At rest before a plunge, the tool off until its duty resets. */
    DWELL: 7,
} as const;
export type JobReason = (typeof JobReason)[keyof typeof JobReason];

/**
 * Which fault ended a home, alongside Fault.HOMING (core0/homing.h).
 *
 * These three were one value until a seek died 190k steps short of its budget
 * and the host confidently reported "switch never reached within 211200 steps"
 * — an accusation against a switch that had done nothing wrong. They are
 * diagnosed in three unrelated places, so they are three values.
 */
export const HomeFail = {
    /** The node stopped itself and the switch never asserted. A genuine "never
     *  reached": bad travel figure, wrong approach direction, or a dead switch. */
    BUDGET: 1,
    /** The node stopped ANSWERING mid-leg (4 polls in a row). Says nothing about
     *  the axis — the bus is the suspect, and the motion may have been fine. */
    POLL: 2,
    /** Still pulsing past the supervisor's own timeout. The node's own budget
     *  should have stopped it first, so this points at the pulser. */
    DEADLINE: 3,
    /** Rotary only. The sweep completed and found no usable feature: it never saw
     *  the magnet (zero crossings), or the captured window reduced to nothing.
     *  Sensor, magnet, or wiring — NOT the budget. */
    INDEX_ABSENT: 4,
    /** Rotary only. A feature was there but would not fit the node's capture
     *  buffer even at the derived decimation. The dip's shape changed; the
     *  detector is working and refusing to trust it. */
    INDEX_SHAPE: 5,
    /** Rotary only. The index repeated at intervals that disagree with each
     *  other. The measurement is sound and the MECHANISM is not: slipped belt,
     *  stalled driver, or a feature that is not once-per-revolution. */
    INDEX_SLIP: 6,
    /** A park leg stopped off its target: on a switch, short of it, or with the
     *  node's datum witness gone. */
    PARK: 8,
    /** The leg was stopped by `leg_abort`. */
    ABORTED: 9,
    /** A `home` run: the node refused a leg mid-run. */
    REFUSED: 10,
    /** A `home` run: the cycle's datum commit failed on this node. */
    DATUM: 11,
    /** A `home` run, rotary: the two sweeps disagree past toleranceDeg, or
     *  measured revolutions more than 2% apart. */
    INDEX_TOL: 12,
} as const;
export type HomeFail = (typeof HomeFail)[keyof typeof HomeFail];
const HOME_FAIL_VALUES = Object.values(HomeFail) as readonly number[];

/** Probe session phase, `probing=` (ProbingReason in ipc/shared_state.h). */
export const Probing = {
    /** A leg is executing. */
    LEG: 0,
    /** Between legs, switch closed: the tool is off the surface. */
    CLEAR: 1,
    /** Between legs, switch open: the tool is on the surface. */
    CONTACT: 2,
} as const;
export type Probing = (typeof Probing)[keyof typeof Probing];

/** Last probe leg's outcome, `probe=` (PROBE_* in ipc/core1_rpc.h). */
export const ProbeCause = {
    OK: 0,
    BUDGET: 1,
    POLL: 2,
    CHATTER: 3,
    ALREADY_OPEN: 4,
    NOT_CLEARED: 5,
    POS_MISMATCH: 6,
    DEADLINE: 7,
    ESTOP: 8,
} as const;
export type ProbeCause = (typeof ProbeCause)[keyof typeof ProbeCause];

// ── axis bitmask — bit0=X bit1=Y bit2=Z bit3=A ─────────────────────────────────

export type AxisLetter = "x" | "y" | "z" | "a";

export const AXIS_BITS: Readonly<Record<AxisLetter, number>> = {
    x: 0x1,
    y: 0x2,
    z: 0x4,
    a: 0x8,
};

/** Mask for a string of axis letters, e.g. "xy" -> 0b0011. */
export function axisMask(axes: string): number {
    let m = 0;
    for (const ch of axes) {
        const bit = AXIS_BITS[ch as AxisLetter];
        if (bit !== undefined) m |= bit;
    }
    return m;
}

// ── enum coercion — tolerant of unknown values from a newer firmware ──────────

function enumFromInt<E extends number>(values: readonly number[], v: number, fallback: E): E {
    return values.includes(v) ? (v as E) : fallback;
}

function toInt(tok: string): number {
    const t = tok.trim();
    return t.toLowerCase().startsWith("0x") ? parseInt(t, 16) : parseInt(t, 10);
}

function enumFromStr<E extends number>(values: readonly number[], raw: string | undefined, fallback: E): E {
    if (raw === undefined) return fallback;
    return enumFromInt(values, toInt(raw), fallback);
}

// ── parsed snapshot ────────────────────────────────────────────────────────────

/** STATUS_RSP `selection`: a head index, or one of these (core0/ops/frames.h). */
export const Selection = {
    /** The laser tip, or the head at (0, 0) without a laser. */
    ANCHOR: 0xfe,
    NONE: 0xff,
} as const;

/** STATUS_RSP `layer` with no job. */
export const NO_LAYER = 0xffff;

/**
 * Parsed snapshot from a `get` reply or a binary STATUS_RSP. Each plane
 * leaves `undefined` what it cannot carry, so a caller can tell "no
 * information" apart from a genuine zero.
 */
export class MachineStatus {
    constructor(
        readonly state: MachineState,
        /** Bound axes homed, slot bits. Text plane only. */
        readonly axesHomed: number | undefined,
        /** Bound axes energised, slot bits. Text plane only. */
        readonly axesEnabled: number | undefined,
        /** The fault mask (Fault bits). */
        readonly faults: number,
        /**
         * The current state's reason byte (JobReason while JOB, the jogging,
         * homing or probing reason in those states), 0 for a state without
         * one. The text plane fills it from `job` only.
         */
        readonly reason: number,
        readonly bufCount: number | undefined = undefined,
        /** Work position x, y, z, a in mm and degrees; NaN where there is no value. */
        readonly wpos: readonly [number, number, number, number] | undefined = undefined,
        /**
         * The job's executing layer and record (an inserted block carries the
         * record it leads into), the resume point while paused. Binary plane
         * only; layer NO_LAYER and record 0 with no job.
         */
        readonly layer: number | undefined = undefined,
        readonly record: number | undefined = undefined,
        /**
         * Axes standing on a latched limit switch. Text plane only — STATUS_RSP
         * has no spare byte, and adding one would change a fixed-length frame
         * whose size the demux checks.
         *
         * `undefined` on a binary sample, following the same rule as `pos` and
         * `bufCount` above: a plane that cannot carry a field reports no
         * information, never a zero. Here the distinction has teeth — 0 means
         * "no axis is on a switch", which a UI would render as safe, and a
         * binary poll has no basis for saying that. The consequential fact (the
         * ALARM) is in `faults`, which both planes carry.
         */
        readonly axesLatched: number | undefined = undefined,
        /**
         * Which fault ended the last home, with Fault.HOMING set. See
         * HomeFail — the three causes are diagnosed in completely different
         * places, and the reason byte alone cannot tell them apart.
         */
        readonly homeFail: HomeFail | undefined = undefined,
        /** Probe session phase; present only while PROBING or with Fault.PROBE. */
        readonly probing: number | undefined = undefined,
        /** Last probe leg's outcome (ProbeCause), alongside `probing`. */
        readonly probeCause: number | undefined = undefined,
        /**
         * Stored contact height of the engaged Z, wire steps. null = not probed;
         * undefined = the reply did not say (binary plane or older firmware).
         */
        readonly probeZ: number | null | undefined = undefined,
        /** Bus node whose leg failed, alongside `homeFail` (0 for a dummy leg). */
        readonly homeNode: number | undefined = undefined,
        /** The cycle a `home` run is in; undefined when no run is. */
        readonly homeCycle: number | undefined = undefined,
        /**
         * Bus nodes holding an origin, bit n = node n, mapped or not. undefined
         * on the binary plane or older firmware.
         */
        readonly nodeHomed: number | undefined = undefined,
        /** What refused a sequence after its ok, with Fault.SEQUENCE set. */
        readonly seqFail: string | undefined = undefined,
        /**
         * The running sequence (SequenceId in core0/ops/sequence.h), or the one
         * that failed (Fault.SEQUENCE, HOMING or PROBE). undefined when neither.
         */
        readonly seq: number | undefined = undefined,
        /** Bus nodes enabled, bit n = node n, mapped or not. */
        readonly nodeEnabled: number | undefined = undefined,
        /** The controlled point: a head index or a Selection value. */
        readonly selection: number | undefined = undefined,
        /** The tool id each head has mounted, head order, 0 for none. */
        readonly tools: readonly number[] | undefined = undefined,
    ) {}

    /** True if any of `mask`'s Fault bits is set. */
    hasFault(mask: number): boolean {
        return (this.faults & mask) !== 0;
    }

    homed(axis: AxisLetter): boolean {
        return !!((this.axesHomed ?? 0) & AXIS_BITS[axis]);
    }

    /**
     * True if `axis` is standing on its limit switch. False on a binary sample,
     * which carries no latch information — check `axesLatched !== undefined`
     * first if the difference between "clear" and "unknown" matters.
     */
    latched(axis: AxisLetter): boolean {
        return !!((this.axesLatched ?? 0) & AXIS_BITS[axis]);
    }

    enabled(axis: AxisLetter): boolean {
        return !!((this.axesEnabled ?? 0) & AXIS_BITS[axis]);
    }

    /** True if every axis in `requiredMask` is homed (the pre-flight/resume gate). */
    allHomed(requiredMask: number): boolean {
        return ((this.axesHomed ?? 0) & requiredMask) === requiredMask;
    }

    /** True if every axis in `requiredMask` is energised (a pre-flight gate). */
    allEnabled(requiredMask: number): boolean {
        return ((this.axesEnabled ?? 0) & requiredMask) === requiredMask;
    }
}

// ── text-plane parser — `get <key> …` reply ───────────────────────────────────

/** `get` markers in place of a value (docs/wire_protocol.md "`get` keys"). */
export const GetMarker = {
    /** The read failed. */
    FAILED: "?",
    /** The key does not apply now. */
    NA: "-",
    /** The firmware does not know the key. */
    UNKNOWN: "!",
} as const;

/**
 * Parse a `get` reply into key → raw value, in reply order. Markers are kept
 * as they are; {@link getValue} reads a key with them folded away. Throws on
 * an `err` reply or a line with no `key=value` token.
 */
export function parseGetReply(line: string): Map<string, string> {
    const out = new Map<string, string>();
    const t = line.trim();
    if (!t.startsWith("err")) {
        for (const tok of t.split(/\s+/)) {
            const eq = tok.indexOf("=");
            if (eq > 0) out.set(tok.slice(0, eq), tok.slice(eq + 1));
        }
    }
    if (out.size === 0) throw new Error(`not a get reply: ${JSON.stringify(line)}`);
    return out;
}

/** A key's value, or `undefined` when absent or a marker. */
export function getValue(reply: ReadonlyMap<string, string>, key: string): string | undefined {
    const v = reply.get(key);
    return v === undefined || v === GetMarker.FAILED || v === GetMarker.NA || v === GetMarker.UNKNOWN
        ? undefined
        : v;
}

/**
 * Build a MachineStatus from a `get` reply over at least `state` and `homed`.
 * Absent keys and markers read as absent fields; `enabled` defaults to 0, and
 * out-of-range enum values fall back to their defaults.
 */
export function statusFromGet(reply: ReadonlyMap<string, string>): MachineStatus {
    const fields: Record<string, string> = {};
    for (const k of reply.keys()) {
        const v = getValue(reply, k);
        if (v !== undefined) fields[k] = v;
    }
    const { state, homed } = fields;
    if (state === undefined || homed === undefined) {
        throw new Error(`get reply lacks state or homed: ${JSON.stringify([...reply])}`);
    }
    return new MachineStatus(
        enumFromStr(MACHINE_STATE_VALUES, state, MachineState.IDLE),
        toInt(homed),
        toInt(fields.enabled ?? "0"),
        toInt(fields.faults ?? "0"),
        fields.job !== undefined && fields.job !== "-" ? toInt(fields.job) : 0,
        undefined, // bufCount: text carries `buf`, the poll reads it binary
        fields.wpos !== undefined ? parseUnits(fields.wpos) : undefined,
        undefined, // layer, record: binary plane only
        undefined,
        // Absent on firmware predating docs/homing.md §2.6. 0 is the right
        // default for that case and not a guess: such firmware has no concept of
        // a latched limit, so no axis can be in one as far as it is concerned.
        fields.latched !== undefined ? toInt(fields.latched) : 0,
        // Absent whenever no completed leg backs it, so `undefined` here is the
        // firmware's own "nothing to report" and not merely an old-firmware
        // fallback -- which is why it does NOT default to 0 the way `latched`
        // above does. Signed, so toInt must not be the hex-tolerant path.
        fields.homefail !== undefined
            ? enumFromStr(HOME_FAIL_VALUES, fields.homefail, HomeFail.BUDGET)
            : undefined,
        fields.probing !== undefined ? parseInt(fields.probing, 10) : undefined,
        fields.probe !== undefined ? parseInt(fields.probe, 10) : undefined,
        fields.probed === undefined
            ? undefined
            : fields.probed === "1" && fields.pz !== undefined
                ? parseInt(fields.pz, 10)
                : null,
        fields.homenode !== undefined ? parseInt(fields.homenode, 10) : undefined,
        fields.homecycle !== undefined ? parseInt(fields.homecycle, 10) : undefined,
        fields.nodehomed !== undefined ? toInt(fields.nodehomed) : undefined,
        fields.seqfail,
        fields.seq !== undefined ? parseInt(fields.seq, 10) : undefined,
        fields.nodeenabled !== undefined ? toInt(fields.nodeenabled) : undefined,
        selectionFromGet(reply.get("head")),
        toolsFromGet(fields.tools),
    );
}

/** `x,y,z,a` with `-` for no value → NaN there. */
function parseUnits(v: string): [number, number, number, number] {
    const p = v.split(",").map((s) => (s === "-" ? NaN : Number(s)));
    return [p[0] ?? NaN, p[1] ?? NaN, p[2] ?? NaN, p[3] ?? NaN];
}

/** `head=<n>|anchor|-` → a selection; undefined when the key is absent. */
function selectionFromGet(head: string | undefined): number | undefined {
    if (head === undefined || head === GetMarker.UNKNOWN || head === GetMarker.FAILED) return undefined;
    if (head === GetMarker.NA) return Selection.NONE;
    if (head === "anchor") return Selection.ANCHOR;
    return parseInt(head, 10);
}

/** `tools=<id|->,…` → one id per head, 0 for none or no such head; undefined when absent. */
function toolsFromGet(tools: string | undefined): number[] | undefined {
    if (tools === undefined || tools === GetMarker.UNKNOWN || tools === GetMarker.FAILED) return undefined;
    const p = tools.split(",");
    return Array.from({ length: STATUS_HEADS }, (_, h) => {
        const t = p[h];
        return t === undefined || t === GetMarker.NA ? 0 : parseInt(t, 10);
    });
}

// ── binary STATUS_RSP (0xA3, 37 bytes) ─────────────────────────────────────────
//
// [0]      magic        0xA3
// [1]      state        u8   MachineState
// [2]      reason       u8   the current state's reason
// [3..4]   faults       u16 LE — Fault bits
// [5..6]   nodeEnabled  u16 LE — bit n = bus node n
// [7..8]   nodeHomed    u16 LE — nodes holding an origin
// [9]      selection    u8   — head index, Selection.ANCHOR, Selection.NONE
// [10..11] tools[2]     u8   — each head's mounted id, head order; 0 none
// [12..27] wpos[4]      f32 LE — x, y, z, a; NaN with no value
// [28..29] bufCount     u16 LE — planner blocks queued
// [30..31] layer        u16 LE — the job's executing record, NO_LAYER
// [32..35] record       u32 LE   and 0 with no job
// [36]     CRC8 over [0..35]

/**
 * Parse a 37-byte STATUS_RSP into a MachineStatus. Throws on bad magic/size/CRC.
 *
 * A retired frame's magic is rejected with a message naming the firmware that
 * emitted it: a host consuming fixed-length frames blind must surface the
 * version skew rather than mis-parse it and desync the stream.
 */
export function parseStatusRsp(data: Uint8Array): MachineStatus {
    if (data.length !== STATUS_RSP_SIZE) {
        throw new Error(`Expected ${STATUS_RSP_SIZE} bytes, got ${data.length}`);
    }
    if (data[0] === MAGIC_STATUS_RSP_V1) {
        throw new Error(
            "Pico is sending the retired 9-byte STATUS_RSP (0xA6) — firmware " +
            "predates docs/comms_architecture.md §4.2. Reflash it.",
        );
    }
    if (data[0] === MAGIC_STATUS_RSP_V2) {
        throw new Error(
            "Pico is sending the retired 31-byte STATUS_RSP (0xA7) — firmware " +
            "predates the node-frame status. Reflash it.",
        );
    }
    if (data[0] === MAGIC_STATUS_RSP_V3) {
        throw new Error(
            "Pico is sending the retired 36-byte STATUS_RSP (0xA4) — firmware " +
            "predates per-head tools. Reflash it.",
        );
    }
    if (data[0] !== MAGIC_STATUS_RSP) {
        throw new Error(`Bad magic: 0x${data[0]!.toString(16).padStart(2, "0")}`);
    }
    if (crc8(data, 0, STATUS_RSP_SIZE - 1) !== data[STATUS_RSP_SIZE - 1]) {
        throw new Error("CRC mismatch");
    }
    const dv = new DataView(data.buffer, data.byteOffset, STATUS_RSP_SIZE);
    return new MachineStatus(
        enumFromInt(MACHINE_STATE_VALUES, dv.getUint8(1), MachineState.IDLE),
        undefined,
        undefined,
        dv.getUint16(3, true),
        dv.getUint8(2),
        dv.getUint16(28, true),
        [dv.getFloat32(12, true), dv.getFloat32(16, true), dv.getFloat32(20, true), dv.getFloat32(24, true)],
        dv.getUint16(30, true),
        dv.getUint32(32, true),
        undefined, // axesLatched … seq: text plane only
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        undefined,
        dv.getUint16(7, true),
        undefined,
        undefined,
        dv.getUint16(5, true),
        dv.getUint8(9),
        Array.from(data.subarray(10, 10 + STATUS_HEADS)),
    );
}

export interface PackStatusRspOptions {
    state: number;
    faults: number;
    reason: number;
    nodeEnabled?: number;
    nodeHomed?: number;
    selection?: number; // default Selection.NONE
    tools?: readonly number[]; // head order; missing heads 0
    wpos?: readonly [number, number, number, number]; // default all NaN
    bufCount?: number;
    layer?: number; // default NO_LAYER
    record?: number;
}

/**
 * Pack a status snapshot into the 37-byte STATUS_RSP wire format. Mainly used
 * by the in-process SimTransport (mirrors host.protocol.link.SimBackend) and
 * by tests; the real Pico emits this frame.
 */
export function packStatusRsp(opts: PackStatusRspOptions): Uint8Array {
    const buf = new ArrayBuffer(STATUS_RSP_SIZE);
    const dv = new DataView(buf);
    const u8 = new Uint8Array(buf);

    dv.setUint8(0, MAGIC_STATUS_RSP);
    dv.setUint8(1, opts.state & 0xff);
    dv.setUint8(2, opts.reason & 0xff);
    dv.setUint16(3, opts.faults & 0xffff, true);
    dv.setUint16(5, (opts.nodeEnabled ?? 0) & 0xffff, true);
    dv.setUint16(7, (opts.nodeHomed ?? 0) & 0xffff, true);
    dv.setUint8(9, (opts.selection ?? Selection.NONE) & 0xff);
    for (let h = 0; h < STATUS_HEADS; h++) dv.setUint8(10 + h, (opts.tools?.[h] ?? 0) & 0xff);
    const w = opts.wpos ?? [NaN, NaN, NaN, NaN];
    for (let i = 0; i < 4; i++) dv.setFloat32(12 + 4 * i, w[i]!, true);
    dv.setUint16(28, (opts.bufCount ?? 0) & 0xffff, true);
    dv.setUint16(30, (opts.layer ?? NO_LAYER) & 0xffff, true);
    dv.setUint32(32, (opts.record ?? 0) >>> 0, true);
    dv.setUint8(36, crc8(u8, 0, STATUS_RSP_SIZE - 1));

    return u8;
}