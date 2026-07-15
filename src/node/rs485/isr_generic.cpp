// isr_generic.cpp — generic RX ISR for non-motion node types (vacuum, oscillator, etc...).
//
// Frames command bytes into cmdQueue and ignores stream bytes (these types are
// not motion participants). Compiled for every type EXCEPT stepper, which owns
// its own RX ISR (types/stepper/stepper.cpp) so the stream path inlines with no
// register-spill. Including this in the stepper build would doubly define the
// USART_RXC vector — a deliberate link-time guardrail.
#include <Arduino.h>
#include "board.h"
#include "rs485/frame.h"

ISR(HAL_USART_RXC_vect) {
    uint8_t status = HAL_USART_INST.RXDATAH;
    uint8_t b      = HAL_USART_INST.RXDATAL;

    if (status & 0x01) {            // 9th bit = 1 → command frame
        frame_command_byte(b);
        return;
    }
    frame_stream_reset();           // 9th bit = 0 → stream byte: ignored
}
