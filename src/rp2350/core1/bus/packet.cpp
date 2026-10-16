// packet.cpp — RS485 frame layer. See packet.h.
#include <Arduino.h>
#include "packet.h"
#include "common.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"      // nodeStatusDecode

RS485Bus rs485;

void busQuiesce(void) {
    while (!rs485.txEmpty());
    rs485.flushRX();
    rs485.writeStream(0);
}

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

bool busNodeCmd(uint8_t node, uint8_t cmd, uint8_t arg) {
    uint8_t pkt[5] = {node, cmd, 1, arg, 0};
    busQuiesce();
    sendPacket(pkt, 5);
    uint8_t buf[RPC_PAYLOAD_MAX];
    uint8_t rxCmd;
    const uint8_t rx = receivePacket(node, cmd, buf, RESPONSE_TIMEOUT_MS, &rxCmd);
    return rx != 0xFF && rxCmd == cmd;
}

bool busNodeSafe(uint8_t node, uint8_t cmd) {
    uint8_t pkt[4] = {node, cmd, 0, 0};
    busQuiesce();
    sendPacket(pkt, 4);
    uint8_t buf[RPC_PAYLOAD_MAX];
    uint8_t rxCmd;
    const uint8_t rx = receivePacket(node, cmd, buf, RESPONSE_TIMEOUT_MS, &rxCmd);
    // Clear only what a node confirmed. One that did not answer keeps its
    // bit: it may still be holding torque.
    if (rx == 0xFF || rxCmd != cmd) return false;
    if (cmd == CMD_DISABLE) { nodeEnabled &= ~(1u << node); return true; }
    NodeStatus st;
    if (nodeStatusDecode(buf, rx, &st) && !(st.flags & NODE_FLAG_ENABLED))
        nodeEnabled &= ~(1u << node);
    return true;
}

void busStopAll(uint8_t cmd) {
    // A node that did not confirm keeps its bit; Core 0 fences its slot on the
    // estop edge and `unstop` asks it again.
    for (uint8_t node = 1; node <= BUS_ADDR_MAX; node++) busNodeSafe(node, cmd);
}
