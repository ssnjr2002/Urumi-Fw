/**
 * packet.ts — stamping the rolling sequence number into a BEZIER record.
 */

import { BEZIER_SEQ_OFFSET, BEZIER_SIZE, MAGIC_BEZIER } from "./constants.js";
import { crc8 } from "./crc.js";

/**
 * Stamp a rolling 8-bit sequence number into a BEZIER record and recompute the
 * CRC. The Pico only executes a record whose seq matches the one it expects
 * next; a stale Go-Back-N retransmit (one it already accepted) is ACKed but NOT
 * executed. Without that, any go-back after the Pico accepted in-flight records
 * would duplicate motion.
 */
export function stampSeq(packet: Uint8Array, seq: number): Uint8Array {
    if (packet[0] !== MAGIC_BEZIER || packet.length !== BEZIER_SIZE) {
        throw new Error("stampSeq: not a BEZIER record");
    }
    const out = new Uint8Array(BEZIER_SIZE);
    out.set(packet.subarray(0, BEZIER_SIZE - 1));
    out[BEZIER_SEQ_OFFSET] = seq & 0xff;
    out[BEZIER_SIZE - 1] = crc8(out, 0, BEZIER_SIZE - 1);
    return out;
}
