#pragma once
#ifndef COMMON_H
#define COMMON_H

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          921600
#define RESPONSE_TIMEOUT_MS    20
#define CMD_PING 0x01
#define CMD_PONG 0x02

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

#endif