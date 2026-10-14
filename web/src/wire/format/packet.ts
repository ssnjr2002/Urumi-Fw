/**
 * packet.ts — stamping the rolling sequence number into a BEZIER record or a
 * LAYER_HDR.
 */

import {
    BEZIER_SEQ_OFFSET, BEZIER_SIZE, MAGIC_BEZIER,
    LAYER_HDR_SEQ_OFFSET, LAYER_HDR_SIZE, MAGIC_LAYER_HDR,
} from "./constants.js";
import { crc8 } from "./crc.js";

/**
 * Stamp a rolling 8-bit sequence number into a BEZIER record or a LAYER_HDR and
 * recompute the CRC. The Pico only executes a record whose seq matches the one it expects
 * next; a stale Go-Back-N retransmit (one it already accepted) is ACKed but NOT
 * executed. Without that, any go-back after the Pico accepted in-flight records
 * would duplicate motion.
 */
export function stampSeq(packet: Uint8Array, seq: number): Uint8Array {
    const bezier = packet[0] === MAGIC_BEZIER && packet.length === BEZIER_SIZE;
    const layer = packet[0] === MAGIC_LAYER_HDR && packet.length === LAYER_HDR_SIZE;
    if (!bezier && !layer) throw new Error("stampSeq: not a BEZIER record or LAYER_HDR");
    const size = packet.length;
    const out = new Uint8Array(size);
    out.set(packet.subarray(0, size - 1));
    out[bezier ? BEZIER_SEQ_OFFSET : LAYER_HDR_SEQ_OFFSET] = seq & 0xff;
    out[size - 1] = crc8(out, 0, size - 1);
    return out;
}
