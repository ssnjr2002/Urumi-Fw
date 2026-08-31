// packet.cpp — RS485 frame layer. See packet.h.
#include <Arduino.h>
#include "packet.h"
#include "common.h"
#include "../../ipc/shared_state.h"

RS485Bus rs485;

void sendPacket(uint8_t* packet, uint8_t len) {
    packet[len - 1] = crc8(packet, len - 1);
    for (int i = 0; i < len; i++) rs485.writeCommand(packet[i]);
}

uint8_t receivePacket(uint8_t expectedNode, uint8_t expectedCmd,
                              uint8_t* outPayload, uint32_t timeoutMs,
                              uint8_t* outCmd) {
    uint32_t start = millis();
    uint8_t  rxBuf[32];
    int      rxIdx = 0;

    while (millis() - start < timeoutMs) {
        if (!rs485.available()) continue;

        uint16_t rcv = rs485.read();
        if (!(rcv & (1 << 8))) { rxIdx = 0; continue; } // stream byte — discard

        rxBuf[rxIdx++] = (uint8_t)(rcv & 0xFF);
        if (rxIdx < 4) continue;

        uint8_t payloadLen      = rxBuf[2];
        int     expectedTotalLen = 3 + payloadLen + 1;
        if (rxIdx < expectedTotalLen) continue;

        // A NAK answers any command (common.h), so it passes the opcode filter
        // alongside the expected reply. The CRC check is unchanged — a refusal is
        // not trusted any further than an ack is.
        bool ok = (rxBuf[0] == expectedNode) &&
                  (rxBuf[1] == expectedCmd || rxBuf[1] == CMD_NAK) &&
                  (rxBuf[rxIdx - 1] == crc8(rxBuf, rxIdx - 1));

        if (ok) {
            if (outCmd) *outCmd = rxBuf[1];
            if (outPayload && payloadLen > 0) memcpy(outPayload, &rxBuf[3], payloadLen);
            return payloadLen;
        }
        rxIdx = 0; // bad packet — restart
    }
    return 0xFF; // timeout
}

bool sendBroadcast(uint8_t cmd) {
    if (!cmdAllowsBroadcast(cmd)) return false;
    uint8_t pkt[4] = {BUS_ADDR_BROADCAST, cmd, 0, 0};
    sendPacket(pkt, 4);
    // No receivePacket: a broadcast is answered by nobody (see common.h).
    return true;
}

void busDisableAll(void) {
    for (uint8_t node = 1; node <= BUS_ADDR_MAX; node++) {
        uint8_t pkt[4] = {node, CMD_DISABLE, 0, 0};
        sendPacket(pkt, 4);
        receivePacket(node, CMD_DISABLE, nullptr, RESPONSE_TIMEOUT_MS);  // consume
    }
}
