// main.cpp — node core skeleton (type-agnostic).
// Brings up the bus and LED, hands type-specific init to node_setup(), then
// drains the command ring into dispatchCommand(). All motor/stream/position
// logic lives in the selected node type (types/<x>/); see node_hooks.h.
#include <Arduino.h>
#include "board.h"
#include "common.h"
#include "protocol.h"
#include "node_hooks.h"
#include "rs485/rs485.h"

// Defined in dispatch.cpp.
void dispatchCommand(const uint8_t* pkt, uint8_t len);

#ifdef NODE_DEBUG_CONSOLE
// Optional USART0 bench console (debug_console.cpp) — bypasses RS485 entirely.
void debugConsoleBegin(void);
void debugConsolePoll(void);
#endif

void setup() {
    #ifdef NODE_DEBUG_CONSOLE
        debugConsoleBegin();   // USART0 (PB2/PB3) — after sei(), independent of USART1
    #endif
    pinMode(HAL_RS485_DE_PIN, OUTPUT); digitalWrite(HAL_RS485_DE_PIN, LOW);
    HAL_USART_TX_IDLE_INIT();
    pinMode(HAL_LED_PIN, OUTPUT); digitalWrite(HAL_LED_PIN, LOW);

    node_setup();          // type-specific: pins, drivers, timers, slot masks
    HAL_USART_INIT();
    sei();

    // Blink NODE_ID times — visual address confirmation.
    uint8_t blinks = (NODE_ID < 6) ? (uint8_t)NODE_ID : 5u;
    for (uint8_t i = 0; i < blinks; i++) {
        digitalWrite(HAL_LED_PIN, HIGH); delay(150);
        digitalWrite(HAL_LED_PIN, LOW);  delay(150);
    }

}

void loop() {
    node_loop();               // type-specific per-iteration work (non-blocking)

#ifdef NODE_DEBUG_CONSOLE
    debugConsolePoll();        // drain any bench-console command line
#endif

    if (cmdHead == cmdTail) return;

    CommandPacket* pkt = &cmdQueue[cmdTail];
    uint8_t len = pkt->length;

    bool validNode = (pkt->data[0] == NODE_ID || pkt->data[0] == 0xFF);
    bool validCrc  = (pkt->data[len - 1] == crc8(pkt->data, len - 1));

    if (validNode && validCrc)
        dispatchCommand(pkt->data, len);

    cmdTail = (cmdTail + 1) % MAX_COMMANDS;
}
