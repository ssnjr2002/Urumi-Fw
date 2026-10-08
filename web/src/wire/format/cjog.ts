/**
 * format/cjog.ts — the continuous-jog deadman packet (docs/wire_protocol.md
 * "Continuous Jog").
 *
 * The host repeats the packet faster than CJOG_DEADMAN_MS while the operator
 * holds a direction; the Pico stops the jog when the packets stop, on all-zero
 * directions, or on MAGIC_CJOG_STOP. A packet moves one axis set — XY, Z or
 * A — or is NACKed NACK_MIXED_AXES.
 */

import { crc8 } from "./crc.js";
import { CJOG_SIZE, CJOG_SPEED_ONE, MAGIC_CJOG } from "./constants.js";

export type JogDir = -1 | 0 | 1;

/** The largest speed multiplier the byte can carry (255/64). */
export const CJOG_SPEED_MAX = 255 / CJOG_SPEED_ONE;

/**
 * Pack a continuous-jog packet. `speed` multiplies the axis's jogFeed, in
 * 1/64 steps; the Pico caps the result at maxFeed.
 */
export function packCjog(x: JogDir, y: JogDir, speed = 1, z: JogDir = 0, a: JogDir = 0): Uint8Array {
    const byte = Math.round(speed * CJOG_SPEED_ONE);
    if (!(byte >= 1 && byte <= 255)) {
        throw new RangeError(`cjog speed ${speed} outside (0, ${CJOG_SPEED_MAX}]`);
    }
    const u8 = new Uint8Array(CJOG_SIZE);
    const dv = new DataView(u8.buffer);
    dv.setUint8(0, MAGIC_CJOG);
    dv.setInt8(1, x);
    dv.setInt8(2, y);
    dv.setInt8(3, z);
    dv.setInt8(4, a);
    dv.setUint8(5, byte);
    u8[CJOG_SIZE - 1] = crc8(u8, 0, CJOG_SIZE - 1);
    return u8;
}
