#pragma once
#ifndef COMMON_H
#define COMMON_H

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          921600
#define RESPONSE_TIMEOUT_MS    20
#define CMD_PING 0x01
#define CMD_PONG 0x02
#define CMD_GET_POS 0x03
#define CMD_ENABLE 0x04
#define CMD_DISABLE 0x05

static inline uint8_t crc8(const uint8_t *data, uint8_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        uint8_t extract = *data++;
        for (uint8_t tempI = 8; tempI; tempI--) {
            uint8_t sum = (crc ^ extract) & 0x01;
            crc >>= 1;
            if (sum) crc ^= 0x8C;
            extract >>= 1;
        }
    }
    return crc;
}

// CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) — stronger collision
// resistance for the config-blob correctness gate. Same value is stored in the
// flash header, returned by CMD_GET_CONFIG, and compared in the Phase 2 MCFG
// handshake (docs/wire_protocol.md). Bitwise (no table) — config writes are rare.
static inline uint32_t crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *data++;
        for (uint8_t k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

#endif