// hello_serial.cpp — minimal standalone serial bring-up probe. NOT part of the
// node firmware. Proves whether a board's USB-serial adapter reaches a given
// USART in BOTH directions, before trusting the real debug console.
//
// The USART is selected at build time via -DHELLO_SERIAL=<instance>:
//   AVR128DB32 (RS485 on USART2): -DHELLO_SERIAL=Serial1  -> USART1, PC0/PC1
//   ATtiny3226 (RS485 on USART1): -DHELLO_SERIAL=Serial   -> USART0, PB2/PB3
// Defaults to Serial if unset. See the hello_serial* envs in platformio.ini.
//
// TX test: prints a rolling counter once a second (node -> PC).
// RX test: echoes typed chars back UPPERCASE as "echo: X" (PC -> node).
#include <Arduino.h>

#ifndef HELLO_SERIAL
#define HELLO_SERIAL Serial
#endif

// Stringify the macro so the boot banner reports which instance was compiled in.
#define HELLO_STR2(x) #x
#define HELLO_STR(x)  HELLO_STR2(x)

void setup() {
    HELLO_SERIAL.begin(115200);
    HELLO_SERIAL.println();
    HELLO_SERIAL.println(F("hello_serial BOOT on " HELLO_STR(HELLO_SERIAL)));
}

void loop() {
    static uint32_t n = 0;
    static uint32_t last = 0;

    if (millis() - last >= 1000) {
        last = millis();
        HELLO_SERIAL.print(F("hello world #"));
        HELLO_SERIAL.println(n++);
    }

    while (HELLO_SERIAL.available()) {
        char c = (char)HELLO_SERIAL.read();
        HELLO_SERIAL.print(F("echo: "));
        HELLO_SERIAL.println((char)toupper(c));
    }
}
