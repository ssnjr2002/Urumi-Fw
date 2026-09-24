/**
 * format/crc.ts — checksums the firmware uses.
 *
 * CRC-8, polynomial 0x8C, init 0x00, reflected — the "Dallas/Maxim" 1-Wire
 * variant the RP2350 firmware and the Python host both use (host/protocol/
 * packets.py `_crc8`). Every wire packet ends in this CRC over its body.
 *
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) guards the config blob,
 * matching crc32() in include/common.h.
 */

/**
 * CRC-8 over data[start..end] (exclusive end). Polynomial 0x8C, init 0x00,
 * reflected — the "Dallas/Maxim" 1-Wire variant the firmware uses.
 *
 * Default range is the whole buffer. The start/end form lets the caller
 * CRC a slice without allocating a subarray.
 */
export function crc8(data: ArrayLike<number>, start = 0, end = data.length): number {
    let crc = 0x00;
    for (let i = start; i < end; i++) {
        crc ^= data[i]!;
        for (let _ = 0; _ < 8; _++) {
            if (crc & 0x01) {
                crc = (crc >> 1) ^ 0x8c;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc & 0xff;
}

/** CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) over the whole buffer. */
export function crc32(data: ArrayLike<number>): number {
    let crc = 0xffffffff;
    for (let i = 0; i < data.length; i++) {
        crc ^= data[i]!;
        for (let _ = 0; _ < 8; _++) {
            crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
        }
    }
    return (crc ^ 0xffffffff) >>> 0;
}