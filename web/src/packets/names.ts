/**
 * packets/names.ts — the wire codes in words, imported as one namespace:
 * `import { names } from "urumi-host/packets"`, then `names.state(s)`.
 *
 * Names are the enum keys, not prose: `names.state(ALARM)` is "ALARM", the
 * token the firmware logs and the wire docs use, so the UI and a capture show
 * the same word. Derived from the enums, never a positional literal, so a
 * reordered enum or a gap can't misname a value.
 *
 * An unknown value prints as `STATE(12)`: a newer firmware may send a code this
 * build has never heard of, and the number survives into the log.
 *
 * The tables are built on first use. status.ts imports this module for the
 * MachineStatus getters, and this one reads its enums, so nothing here may
 * touch them while the modules are still loading.
 */

import {
    MachineState,
    MachineContext,
    Fault,
    JobReason,
    JoggingReason,
    HomingReason,
    ProbingReason,
    Selection,
    AXIS_BITS,
    type AxisLetter,
} from "./status.js";
import { Nack } from "./nack.js";
import { CfgNack } from "./cfg.js";

type Table = Readonly<Record<number, string>>;

/** value → key, for the const-object enums TypeScript gives no reverse mapping. */
function invert(obj: Record<string, number>): Table {
    const out: Record<number, string> = {};
    for (const [k, v] of Object.entries(obj)) out[v] = k;
    return out;
}

const cache = new Map<object, Table>();
function lookup(obj: Record<string, number>, v: number, unknown: string): string {
    let t = cache.get(obj);
    if (!t) cache.set(obj, (t = invert(obj)));
    return t[v] ?? unknown;
}

/** A MachineState, e.g. "ALARM". */
export const state = (s: number): string => lookup(MachineState, s, `STATE(${s})`);

/**
 * The reason byte, read against the state it came with: JOB's "PAUSED",
 * JOGGING's "GOTO". "" in a state without a reason.
 */
export function reason(st: number, r: number): string {
    switch (st) {
        case MachineState.JOB: return lookup(JobReason, r, `REASON(${r})`);
        case MachineState.JOGGING: return lookup(JoggingReason, r, `REASON(${r})`);
        case MachineState.HOMING: return lookup(HomingReason, r, `REASON(${r})`);
        case MachineState.PROBING: return lookup(ProbingReason, r, `REASON(${r})`);
        default: return "";
    }
}

/** A MachineContext, e.g. "JOB". */
export const context = (c: number): string => lookup(MachineContext, c, `CONTEXT(${c})`);

/** The set bits of a fault mask, e.g. "UNMAPPED|LATCHED"; "NONE" for 0. */
export function faults(mask: number): string {
    const out: string[] = [];
    for (let bit = 1; bit <= 0x8000; bit <<= 1)
        if (mask & bit) out.push(lookup(Fault, bit, `0x${bit.toString(16)}`));
    return out.length ? out.join("|") : "NONE";
}

/** A Nack code, from a NACK or a JOG_NACK, e.g. "FULL". */
export const nack = (code: number): string => lookup(Nack, code, `NACK(${code})`);

/** A CfgNack code, e.g. "SCHEMA". */
export const cfgNack = (code: number): string => lookup(CfgNack, code, `CFG_NACK(${code})`);

/** The controlled point: "ANCHOR", "NONE", or a head index such as "0". */
export const selection = (s: number): string => lookup(Selection, s, String(s));

const LETTERS: readonly AxisLetter[] = ["x", "y", "z", "a"];

/** An axis mask as letters in xyza order: 0b1011 → "xya"; "—" for none. */
export function axes(mask: number): string {
    return LETTERS.filter((l) => mask & AXIS_BITS[l]).join("") || "—";
}
