/**
 * format/crc.ts — checksums the firmware uses.
 *
 * CRC-8, polynomial 0x8C, init 0x00, reflected — the "Dallas/Maxim" 1-Wire
 * variant the RP2350 firmware and the Python host both use (host/protocol/
 * packets.py `_crc8`). Every wire packet ends in this CRC over its body.
 *
 * A CRC-32 placeholder is reserved for the Phase-2 config-blob integrity
 * check (docs/wire_protocol.md "CRC Algorithms"); it lands with the cfg
 * packer.
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