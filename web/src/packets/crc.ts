/**
 * format/crc.ts — checksums the firmware uses.
 *
 * CRC-8, polynomial 0x8C, init 0x00, reflected — the "Dallas/Maxim" 1-Wire
 * variant the RP2350 firmware uses. Every wire packet ends in this CRC over
 * its body.
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

const CRC32_TABLE = (() => {
    const t = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
        let c = n;
        for (let k = 0; k < 8; k++) c = c & 1 ? (c >>> 1) ^ 0xedb88320 : c >>> 1;
        t[n] = c >>> 0;
    }
    return t;
})();

/**
 * CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) over data[start..end]
 * (exclusive end). Pass a previous result as `crc` to continue it over more
 * data: crc32(b, 0, b.length, crc32(a)) equals the CRC of a then b.
 */
export function crc32(data: ArrayLike<number>, start = 0, end = data.length, crc = 0): number {
    let c = ~crc >>> 0;
    for (let i = start; i < end; i++) c = CRC32_TABLE[(c ^ data[i]!) & 0xff]! ^ (c >>> 8);
    return ~c >>> 0;
}
