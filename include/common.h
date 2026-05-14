#pragma once
#ifndef COMMON_H
#define COMMON_H

// ─── Bus ───────────────────────────────────────────────────────────────────────
#define RS485_BAUD          115200
#define RESPONSE_TIMEOUT_MS    20
#define POLL_INTERVAL_MS       10

// ─── Protocol ──────────────────────────────────────────────────────────────────
#define BROADCAST    0xFF
#define RESP_CHAR    0xFD
#define STATUS_OK    0x00
#define STATUS_DATA  0x01
#define STATUS_ERR   0xFF

#define CMD_PING            0x01
#define CMD_QUEUE           0x02
#define CMD_GO              0x03
#define CMD_STOP            0x04
#define CMD_STATUS          0x05
#define CMD_ENABLE          0x06
#define CMD_QUEUE_DUMMY     0x07

#endif