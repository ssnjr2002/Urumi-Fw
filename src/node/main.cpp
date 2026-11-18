// main.cpp — the node core's setup() and loop(), type-agnostic.
// Brings up the bus and LED, calls node_setup(), then drains the command queue
// into dispatchCommand(). See docs/system/nodes.md §3.
#include <Arduino.h>
#include "board.h"
#include "common.h"
#include "protocol.h"
#include "node_hooks.h"
#include "rs485/rs485.h"

// Defined in dispatch.cpp.
void dispatchCommand(const uint8_t* pkt, uint8_t len, bool broadcast);

#ifdef NODE_DEBUG_CONSOLE
// Bench console on the board's free USART (debug_console.cpp).
void debugConsoleBegin(void);
void debugConsolePoll(void);
#endif

void setup() {
    #ifdef NODE_DEBUG_CONSOLE
        debugConsoleBegin();
    #endif
    pinMode(HAL_RS485_DE_PIN, OUTPUT); digitalWrite(HAL_RS485_DE_PIN, LOW);
    HAL_USART_TX_IDLE_INIT();
    pinMode(HAL_LED_PIN, OUTPUT); digitalWrite(HAL_LED_PIN, LOW);

    node_setup();
    HAL_USART_INIT();
    sei();

    // Blink NODE_ID times, to show the address.
    uint8_t blinks = (NODE_ID < 8) ? (uint8_t)NODE_ID : 0;
    for (uint8_t i = 0; i < blinks; i++) {
        digitalWrite(HAL_LED_PIN, HIGH); delay(150);
        digitalWrite(HAL_LED_PIN, LOW);  delay(150);
    }

}

#ifdef NODE_HAS_SILENCE_TIMEOUT
// Make safe once after BUS_SILENCE_MS without a byte; the next byte re-arms it.
static void busSilenceCheck(void) {
    static uint32_t lastHeardMs = 0;
    static bool     safe        = false;
    if (busHeard) {
        busHeard    = false;
        lastHeardMs = millis();
        safe        = false;
    } else if (!safe && millis() - lastHeardMs >= BUS_SILENCE_MS) {
        node_make_safe();
        safe = true;
    }
}
#endif

void loop() {
    node_loop();

#ifdef NODE_HAS_SILENCE_TIMEOUT
    busSilenceCheck();
#endif

#ifdef NODE_DEBUG_CONSOLE
    debugConsolePoll();
#endif

    if (cmdHead == cmdTail) return;

    CommandPacket* pkt = &cmdQueue[cmdTail];
    uint8_t len = pkt->length;

    bool broadcast = (pkt->data[0] == BUS_ADDR_BROADCAST);
    bool validNode = (pkt->data[0] == NODE_ID || broadcast);
    bool validCrc  = (pkt->data[len - 1] == crc8(pkt->data, len - 1));

    if (validNode && validCrc)
        dispatchCommand(pkt->data, len, broadcast);
    else if (validNode)
        busCrcCount++;

    cmdTail = (cmdTail + 1) % MAX_COMMANDS;
}
