/**
 * wire/format/names.ts — human-readable names for the wire enums.
 *
 * Every UI needs these and every UI wrote its own. Two shapes were in use:
 * `invert(MachineState)` (correct, in comms.js) and a positional literal
 * `['IDLE','RUNNING','ESTOP','ALARM','PAUSED','HOMING']` (bench.js,
 * orchestrate.js) which is only right for as long as nobody reorders the enum
 * or leaves a gap in it. Both existed in the same folder.
 *
 * Placed beside the enums rather than in machine/, per the placement rule: a
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
    AlarmReason,
    RunningReason,
    AXIS_BITS,
    type AxisLetter,
} from "./status.js";

/** value → key, for the const-object "enums" TS gives no reverse mapping. */
function invert(obj: Record<string, number>): Record<number, string> {
    const out: Record<number, string> = {};
    for (const [k, v] of Object.entries(obj)) out[v] = k;
    return out;
}

export const STATE_NAMES: Readonly<Record<number, string>> = invert(MachineState);
export const ALARM_NAMES: Readonly<Record<number, string>> = invert(AlarmReason);
export const RUNNING_NAMES: Readonly<Record<number, string>> = invert(RunningReason);

/**
 * Unknown values fall back to `STATE(7)` rather than "" or "undefined".
 * A newer firmware may report a state this build has never heard of — the
 * parsers are already tolerant of that (enumFromInt), so the printers are too,
 * and the number survives into the log where it can be looked up.
 */
export const stateName = (s: number): string => STATE_NAMES[s] ?? `STATE(${s})`;
export const alarmName = (a: number): string => ALARM_NAMES[a] ?? `ALARM(${a})`;
export const runningName = (r: number): string => RUNNING_NAMES[r] ?? `RUNNING(${r})`;

const LETTERS: readonly AxisLetter[] = ["x", "y", "z", "a"];

/**
 * An axis bitmask as letters: 0b1011 -> "xya". Empty renders as an em dash, not
 * "", so a status row reads "homed: —" rather than looking like a render bug.
 */
export function maskStr(mask: number): string {
    return LETTERS.filter((l) => mask & AXIS_BITS[l]).join("") || "—";
}
