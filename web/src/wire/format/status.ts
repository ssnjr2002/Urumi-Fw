/**
 * format/status.ts — binary STATUS_RSP (0xA7, 30B) + the operational enums.
 * Ported from host/protocol/{state,packets}.py: MachineState / AlarmReason /
 * RunningReason, MachineStatus, parse_getstate, parse_status_rsp,
 * pack_status_rsp, axis_mask.
 *
 * STATUS_RSP folds state + position + expectedSeq + queued motion time into
 * one coherent sample (docs/comms_architecture.md §4.2/§4.6). It supersedes
 * the text `getstate` + `getpos` pair for any caller that can do a binary
 * poll — including mid-stream, where the ASCII line would be a heavier
 * insertion between MSEG packets. ASCII `getstate` remains for bring-up and
 * human use; parseGetstate keeps parity with it.
 *
 * The fields text cannot carry (`pos`, `expectedSeq`, `queuedUs`, `bufCount`)
 * stay `undefined` on a getstate-parsed MachineStatus, distinguishing "this
 * came from the text plane" from a real zero.
 */

import { crc8 } from "./crc.js";
import {
    MAGIC_STATUS_RSP,
    MAGIC_STATUS_RSP_V1,
    STATUS_RSP_SIZE,
} from "./constants.js";

// ── operational enums (mirror firmware values; docs/wire_protocol.md) ──────────

export const MachineState = {
    IDLE: 0,
    RUNNING: 1,
    ESTOP: 2, // transient inter-core flush signal; rarely seen by the host
    ALARM: 3,
    PAUSED: 4,
    HOMING: 5,
} as const;
export type MachineState = (typeof MachineState)[keyof typeof MachineState];
const MACHINE_STATE_VALUES = Object.values(MachineState) as readonly number[];

export const AlarmReason = {
    NONE: 0,
    ESTOP: 1,
    CONFIG: 2, // Phase 2
    SOFT_LIMIT: 3,
    HOMING_FAIL: 4,
    NODE_FAULT: 5,
    /**
     * An axis is standing on a latched limit switch — where legs 1 and 3 of a
     * home are SUPPOSED to end (docs/homing.md §2.6). Not a fault, but a real
     * alarm: the node refuses stream steps while its limit is latched, so a job
     * admitted here would run the other axes and silently drop this one.
     *
     * This member is load-bearing, not decorative. enumFromInt() coerces an
     * unrecognised value to NONE, so a host missing this entry does not render
     * "ALARM(6)" — it renders NO ALARM AT ALL, on a machine that is alarmed and
     * refusing motion. It must be added in lockstep with the firmware.
     */
    LIMIT_LATCHED: 6,
} as const;
export type AlarmReason = (typeof AlarmReason)[keyof typeof AlarmReason];
const ALARM_REASON_VALUES = Object.values(AlarmReason) as readonly number[];

export const RunningReason = {
    JOB: 0,
    JOG: 1,
    // Decelerating to rest after a pause/abort request (§4.5). A RunningReason
    // and not a MachineState: the machine IS running, so every existing
    // IDLE/RUNNING/PAUSED gate stays correct, and an older host reads it as
    // plain RUNNING.
    ABORT_DECEL: 2,
} as const;
export type RunningReason = (typeof RunningReason)[keyof typeof RunningReason];
const RUNNING_REASON_VALUES = Object.values(RunningReason) as readonly number[];

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

/**
 * Parsed snapshot from a `getstate` reply or a binary STATUS_RSP. The binary
 * form is a strict superset: it also carries `bufCount`, `pos`, `expectedSeq`
 * and `queuedUs`, which text cannot express. Those stay `undefined` on a
 * getstate-parsed status so a caller can tell "no information" apart from a
 * genuine zero.
 */
export class MachineStatus {
    constructor(
        readonly state: MachineState,
        readonly axesHomed: number,
        readonly axesEnabled: number,
        readonly alarm: AlarmReason,
        readonly running: RunningReason,
        readonly bufCount: number | undefined = undefined,
        readonly pos: readonly [number, number, number, number] | undefined = undefined,
        readonly expectedSeq: number | undefined = undefined,
        readonly queuedUs: number | undefined = undefined,
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
         * ALARM) is in `alarm`, which both planes carry.
         */
        readonly axesLatched: number | undefined = undefined,
    ) {}

    homed(axis: AxisLetter): boolean {
        return !!(this.axesHomed & AXIS_BITS[axis]);
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
        return !!(this.axesEnabled & AXIS_BITS[axis]);
    }

    /** True if every axis in `requiredMask` is homed (the pre-flight/resume gate). */
    allHomed(requiredMask: number): boolean {
        return (this.axesHomed & requiredMask) === requiredMask;
    }

    /** True if every axis in `requiredMask` is energised (a pre-flight gate). */
    allEnabled(requiredMask: number): boolean {
        return (this.axesEnabled & requiredMask) === requiredMask;
    }
}

// ── text-plane parser — `getstate` reply ───────────────────────────────────────

/**
 * Parse a `getstate` reply line:
 *     state=<s> enabled=<hex> homed=<hex> alarm=<a> running=<r> latched=<hex>
 *
 * Key=value tokens, space-separated. Tolerant of unknown trailing tokens
 * (forward-compatible) and of out-of-range enum values. Requires at least
 * `state` and `homed`; `enabled` defaults to 0 if absent. Throws if the line
 * is not a status reply.
 */
export function parseGetstate(line: string): MachineStatus {
    const fields: Record<string, string> = {};
    for (const tok of line.trim().split(/\s+/)) {
        const eq = tok.indexOf("=");
        if (eq > 0) {
            fields[tok.slice(0, eq)] = tok.slice(eq + 1);
        }
    }
    if (fields.state === undefined || fields.homed === undefined) {
        throw new Error(`not a getstate reply: ${JSON.stringify(line)}`);
    }
    return new MachineStatus(
        enumFromStr(MACHINE_STATE_VALUES, fields.state, MachineState.IDLE),
        toInt(fields.homed),
        toInt(fields.enabled ?? "0"),
        enumFromStr(ALARM_REASON_VALUES, fields.alarm, AlarmReason.NONE),
        enumFromStr(RUNNING_REASON_VALUES, fields.running, RunningReason.JOB),
        undefined, // bufCount, pos, expectedSeq, queuedUs — text cannot carry these
        undefined,
        undefined,
        undefined,
        // Absent on firmware predating docs/homing.md §2.6. 0 is the right
        // default for that case and not a guess: such firmware has no concept of
        // a latched limit, so no axis can be in one as far as it is concerned.
        fields.latched !== undefined ? toInt(fields.latched) : 0,
    );
}

// ── binary STATUS_RSP (0xA7, 30 bytes) ─────────────────────────────────────────
//
// [0]      magic        0xA7
// [1]      state        u8   MachineState
// [2]      axes_enabled u8   bitmask
// [3]      axes_homed   u8   bitmask
// [4]      alarm        u8   AlarmReason
// [5]      running      u8   RunningReason
// [6..7]   bufCount     u16 LE — segments queued (incl. executing)
// [8..23]  pos[4]       i32 LE — x, y, z, a (steps)
// [24]     expectedSeq  u8   — informational, not flow control
// [25..28] queuedUs     u32 LE — queued motion time, microseconds
// [29]     CRC8 over [0..28]

/**
 * Parse a 30-byte STATUS_RSP into a MachineStatus. Throws on bad magic/size/CRC.
 *
 * The retired 0xA6 (9-byte v1) frame is detected explicitly and rejected with
 * a message naming the firmware that emitted it — a host consuming fixed-length
 * frames blind must surface the version skew rather than mis-parse 30 bytes as
 * 9 and desync the stream.
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
    if (data[0] !== MAGIC_STATUS_RSP) {
        throw new Error(`Bad magic: 0x${data[0]!.toString(16).padStart(2, "0")}`);
    }
    if (crc8(data, 0, STATUS_RSP_SIZE - 1) !== data[STATUS_RSP_SIZE - 1]) {
        throw new Error("CRC mismatch");
    }
    const dv = new DataView(data.buffer, data.byteOffset, STATUS_RSP_SIZE);
    return new MachineStatus(
        enumFromInt(MACHINE_STATE_VALUES, dv.getUint8(1), MachineState.IDLE),
        dv.getUint8(3),
        dv.getUint8(2),
        enumFromInt(ALARM_REASON_VALUES, dv.getUint8(4), AlarmReason.NONE),
        enumFromInt(RUNNING_REASON_VALUES, dv.getUint8(5), RunningReason.JOB),
        dv.getUint16(6, true),
        [dv.getInt32(8, true), dv.getInt32(12, true), dv.getInt32(16, true), dv.getInt32(20, true)],
        dv.getUint8(24),
        dv.getUint32(25, true),
    );
}

export interface PackStatusRspOptions {
    state: number;
    axesEnabled: number;
    axesHomed: number;
    alarm: number;
    running: number;
    bufCount?: number;
    pos?: readonly [number, number, number, number];
    expectedSeq?: number;
    queuedUs?: number;
}

/**
 * Pack a status snapshot into the 30-byte STATUS_RSP wire format. Mainly used
 * by the in-process SimTransport (mirrors host.protocol.link.SimBackend) and
 * by tests; the real Pico emits this frame.
 */
export function packStatusRsp(opts: PackStatusRspOptions): Uint8Array {
    const buf = new ArrayBuffer(STATUS_RSP_SIZE);
    const dv = new DataView(buf);
    const u8 = new Uint8Array(buf);

    dv.setUint8(0, MAGIC_STATUS_RSP);
    dv.setUint8(1, opts.state & 0xff);
    dv.setUint8(2, opts.axesEnabled & 0xff);
    dv.setUint8(3, opts.axesHomed & 0xff);
    dv.setUint8(4, opts.alarm & 0xff);
    dv.setUint8(5, opts.running & 0xff);
    dv.setUint16(6, (opts.bufCount ?? 0) & 0xffff, true);
    const p = opts.pos ?? [0, 0, 0, 0];
    dv.setInt32(8, p[0] | 0, true);
    dv.setInt32(12, p[1] | 0, true);
    dv.setInt32(16, p[2] | 0, true);
    dv.setInt32(20, p[3] | 0, true);
    dv.setUint8(24, (opts.expectedSeq ?? 0) & 0xff);
    dv.setUint32(25, (opts.queuedUs ?? 0) >>> 0, true);
    dv.setUint8(29, crc8(u8, 0, STATUS_RSP_SIZE - 1));

    return u8;
}