/**
 * link/names.ts — human-readable names for the wire enums.
 *
 * Every UI needs these and every UI wrote its own. Two shapes were in use:
 * `invert(MachineState)` (correct, in comms.js) and a positional literal
 * `['IDLE','RUNNING','ESTOP','ALARM','PAUSED','HOMING']` (bench.js,
 * orchestrate.js) which is only right for as long as nobody reorders the enum
 * or leaves a gap in it. Both existed in the same folder.
 *
 * Placed beside the enums rather than in config/, per the placement rule: a
 * name for MachineState depends on MachineState and on nothing else, and a
 * consumer holding only a Link — with no MachineConfig anywhere — still needs
 * to print "ALARM".
 *
 * Names are the enum keys, not prose. `stateName(ALARM)` is "ALARM", the same
 * token the firmware logs and the wire docs use, so an operator reading the UI
 * and an engineer reading a capture are looking at the same word.
 */

import {
    MachineState,
    Fault,
    JobReason,
    AXIS_BITS,
    type AxisLetter,
} from "../packets/status.js";
import {
    NACK_CRC, NACK_FULL, NACK_BAD_MAGIC, NACK_PAUSED, NACK_BAD_STATE, NACK_ABORTING,
    NACK_BAD_CURVE, NACK_SOFT_LIMIT, NACK_MIXED_AXES, NACK_BUSY, NACK_JOB,
    NACK_TOOL_UNKNOWN, NACK_TOOL_CHANGE, NACK_TOOL_UNAVAILABLE, NACK_OUT_OF_RANGE, NACK_NOT_HOMED,
} from "../packets/constants.js";
import {
    CFG_NACK_CRC, CFG_NACK_TOO_BIG, CFG_NACK_BAD_STATE, CFG_NACK_FLASH, CFG_NACK_TIMEOUT, CFG_NACK_SCHEMA,
} from "../packets/cfg.js";

/** value → key, for the const-object "enums" TS gives no reverse mapping. */
function invert(obj: Record<string, number>): Record<number, string> {
    const out: Record<number, string> = {};
    for (const [k, v] of Object.entries(obj)) out[v] = k;
    return out;
}

export const STATE_NAMES: Readonly<Record<number, string>> = invert(MachineState);
export const FAULT_NAMES: Readonly<Record<number, string>> = invert(Fault);
export const JOB_NAMES: Readonly<Record<number, string>> = invert(JobReason);

/**
 * Unknown values fall back to `STATE(7)` rather than "" or "undefined".
 * A newer firmware may report a state this build has never heard of — the
 * parsers are already tolerant of that (enumFromInt), so the printers are too,
 * and the number survives into the log where it can be looked up.
 */
export const stateName = (s: number): string => STATE_NAMES[s] ?? `STATE(${s})`;
/** The set bits of a fault mask, e.g. "UNMAPPED|LATCHED"; "NONE" for 0. */
export const faultNames = (mask: number): string => {
    const out: string[] = [];
    for (let bit = 1; bit <= 0x8000; bit <<= 1)
        if (mask & bit) out.push(FAULT_NAMES[bit] ?? `0x${bit.toString(16)}`);
    return out.length ? out.join("|") : "NONE";
};
export const jobName = (r: number): string => JOB_NAMES[r] ?? `JOB(${r})`;

const LETTERS: readonly AxisLetter[] = ["x", "y", "z", "a"];

/**
 * An axis bitmask as letters: 0b1011 -> "xya". Empty renders as an em dash, not
 * "", so a status row reads "homed: —" rather than looking like a render bug.
 */
export function maskStr(mask: number): string {
    return LETTERS.filter((l) => mask & AXIS_BITS[l]).join("") || "—";
}

const NACK_NAMES: Readonly<Record<number, string>> = {
    [NACK_CRC]: "CRC",
    [NACK_FULL]: "FULL",
    [NACK_BAD_MAGIC]: "BAD_MAGIC",
    [NACK_PAUSED]: "PAUSED",
    [NACK_BAD_STATE]: "BAD_STATE",
    [NACK_ABORTING]: "ABORTING",
    [NACK_BAD_CURVE]: "BAD_CURVE",
    [NACK_SOFT_LIMIT]: "SOFT_LIMIT",
    [NACK_MIXED_AXES]: "MIXED_AXES",
    [NACK_BUSY]: "BUSY",
    [NACK_JOB]: "JOB",
    [NACK_TOOL_UNKNOWN]: "TOOL_UNKNOWN",
    [NACK_TOOL_CHANGE]: "TOOL_CHANGE",
    [NACK_TOOL_UNAVAILABLE]: "TOOL_UNAVAILABLE",
    [NACK_OUT_OF_RANGE]: "OUT_OF_RANGE",
    [NACK_NOT_HOMED]: "NOT_HOMED",
};

const CFG_NACK_NAMES: Readonly<Record<number, string>> = {
    [CFG_NACK_CRC]: "CRC",
    [CFG_NACK_TOO_BIG]: "TOO_BIG",
    [CFG_NACK_BAD_STATE]: "BAD_STATE",
    [CFG_NACK_FLASH]: "FLASH",
    [CFG_NACK_TIMEOUT]: "TIMEOUT",
    [CFG_NACK_SCHEMA]: "SCHEMA",
};

/** A NACK_* code (a record or jog refusal) without its prefix, e.g. "FULL". */
export const nackName = (code: number): string => NACK_NAMES[code] ?? `NACK(${code})`;
/** A CFG_NACK_* code (a config refusal) without its prefix, e.g. "SCHEMA". */
export const cfgNackName = (code: number): string => CFG_NACK_NAMES[code] ?? `CFG_NACK(${code})`;
