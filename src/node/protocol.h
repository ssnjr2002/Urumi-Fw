#pragma once
#include <stdint.h>

#define MAX_COMMANDS   4
#define MAX_PACKET_LEN 32

typedef struct {
    uint8_t data[MAX_PACKET_LEN];
    uint8_t length;
} CommandPacket;
