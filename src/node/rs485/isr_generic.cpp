// isr_generic.cpp — the RX ISR for every type but the stepper.
//
// Frames command bytes into cmdQueue and ignores stream bytes. The stepper has
// its own ISR (types/stepper/stepper.cpp); a build with both defines the vector
// twice and fails to link.
#include <Arduino.h>
#include "board.h"
#include "rs485/frame.h"

// A vacuum built for probing answers stream bytes, here rather than in loop(),
// where the SSR state machine would sit in the reply's path.
#ifdef NODE_HAS_PROBE_REPLY
#include "types/vacuum/probe_slot.h"
#endif

ISR(HAL_USART_RXC_vect) {
    uint8_t status = HAL_USART_INST.RXDATAH;
    uint8_t b      = HAL_USART_INST.RXDATAL;
    if (frame_rx_reject(status)) return;

    if (status & 0x01) {            // 9th bit = 1 → command frame
        frame_command_byte(b);
        return;
    }
    frame_stream_reset();           // 9th bit = 0 → stream byte
#ifdef NODE_HAS_PROBE_REPLY
    probe_stream_byte(b);           // … answered, if it is addressed to our slot
#else
    (void)b;                        // … otherwise ignored
#endif
}
