/**
 * packets/nack.ts — the controller's refusals (docs/reference/host-api/packets.md).
 *
 * A NACK refuses a job record: [0xBC] [code] [len] [text] [crc8]. The text
 * words the check that failed, on a code that ends the job, and is for
 * display: match the code, never the words. A JOG_NACK refuses a jog:
 * [0xB7] [code], no text.
 */

import { crc8 } from "./crc.js";
import { MAGIC_JOG_NACK, MAGIC_NACK } from "./constants.js";

/** Why the controller refused a record or a jog. The firmware's MSEG_NACK_* values. */
export const Nack = {
    CRC: 0x01,
    FULL: 0x02,
    PAUSED: 0x04,
    BAD_STATE: 0x06,
    ABORTING: 0x07,
    BAD_CURVE: 0x08,
    MIXED_AXES: 0x0a,
    BUSY: 0x0b,
    TOOL_LIST: 0x0c,
    TOOL_UNKNOWN: 0x0d,
    TOOL_CHANGE: 0x0f,
    TOOL_UNAVAILABLE: 0x10,
    OUT_OF_RANGE: 0x11,
    NOT_HOMED: 0x12,
    NO_CONFIG: 0x13,
    ORDER: 0x14,
    BAD_FIELD: 0x15,
    NO_RESUME: 0x16,
} as const;
export type Nack = (typeof Nack)[keyof typeof Nack];

/** The longest text a NACK carries, in bytes. */
export const NACK_TEXT_MAX = 48;
/** A NACK's bytes before its text: magic, code, len. */
export const NACK_HDR_SIZE = 3;

/** A refusal: its code, and on a NACK the check that failed, in words. */
export class Refusal {
    constructor(
        readonly code: Nack,
        readonly text: string = "",
    ) {}
}

/** A whole NACK or JOG_NACK as a Refusal. */
export function parseNack(record: Uint8Array): Refusal {
    if (record[0] === MAGIC_JOG_NACK && record.length === 2) return new Refusal(record[1] as Nack);
    if (record[0] !== MAGIC_NACK || record.length < NACK_HDR_SIZE + 1) {
        throw new Error("parseNack: not a NACK or JOG_NACK");
    }
    const len = record[2]!;
    if (record.length !== NACK_HDR_SIZE + len + 1) throw new Error("parseNack: length does not match");
    if (crc8(record, 0, record.length - 1) !== record[record.length - 1]) {
        throw new Error("parseNack: CRC8 mismatch");
    }
    let text = "";
    for (let i = NACK_HDR_SIZE; i < NACK_HDR_SIZE + len; i++) text += String.fromCharCode(record[i]!);
    return new Refusal(record[1] as Nack, text);
}

/** A NACK, for a simulator or a test. */
export function packNack(code: Nack, text = ""): Uint8Array {
    if (text.length > NACK_TEXT_MAX || !/^[\x20-\x7e]*$/.test(text)) {
        throw new RangeError(`packNack: text must be at most ${NACK_TEXT_MAX} printable ASCII bytes`);
    }
    const out = new Uint8Array(NACK_HDR_SIZE + text.length + 1);
    out[0] = MAGIC_NACK;
    out[1] = code;
    out[2] = text.length;
    for (let i = 0; i < text.length; i++) out[NACK_HDR_SIZE + i] = text.charCodeAt(i);
    out[out.length - 1] = crc8(out, 0, out.length - 1);
    return out;
}

/** A JOG_NACK, for a simulator or a test. */
export function packJogNack(code: Nack): Uint8Array {
    return Uint8Array.of(MAGIC_JOG_NACK, code);
}
