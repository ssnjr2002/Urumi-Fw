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

// Non-motion types ignore stream bytes -- except a vacuum carrying the probe
// switch, which answers them (docs/tool_probe.md §4.2). The reply must run here
// and not in loop(), because loop() runs the SSR burst-fire machine and would
// put a variable-duration task directly in the reply path.
#ifdef NODE_HAS_PROBE_REPLY
#include "types/vacuum/probe_slot.h"
#endif

ISR(HAL_USART_RXC_vect) {
    uint8_t status = HAL_USART_INST.RXDATAH;
    uint8_t b      = HAL_USART_INST.RXDATAL;

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
