// probe_slot.h — the vacuum's stream-slot participation, for the tool-height
// probe (docs/tool_probe.md §3.3, §4.1, §4.2).
//
// The bed-floor probe switch is wired to the vacuum node, not to Z, so the Pico
// has to close the probe's control loop across the bus. It does that by binding
// the vacuum into a motion slot and setting that slot's STEP bit on a stream
// byte to mean "report the switch". The vacuum answers with one stream byte.
//
// This lives in a header because the code has two homes: the state and the
// CMD_ENGAGE handler belong to vacuum.cpp (loop context), while the reply must
// run inside the RX ISR (rs485/isr_generic.cpp) — see §4.2. loop() runs the SSR
// burst-fire machine, a variable-duration task, and putting it in the reply path
// is exactly the jitter the probe cannot afford.
//
// Compiled only under -DNODE_HAS_PROBE_REPLY, so the knife and any other type
// sharing isr_generic.cpp pay nothing.
#pragma once
#include <Arduino.h>
#include "board.h"
#include "common.h"
#include "vacuum/vacuum.h"      // HAL_VACUUM_SWITCH_PIN, via the board's -I

// Written by the CMD_ENGAGE handler in loop context, read in the ISR → volatile.
// Boots disengaged: probeStepMask 0 means every stream byte is ignored, exactly
// as an unengaged stepper ignores them.
extern volatile uint8_t probeSlot;
extern volatile uint8_t probeStepMask;
extern volatile uint8_t probeDirMask;

// Answer one stream byte. Called from the RX ISR with the 9th bit already known
// to be 0.
static inline void probe_stream_byte(uint8_t b) {
    // Not bound, or this byte is not asking us. probeStepMask is 0 while
    // disengaged, so the disengaged case falls out of the same test.
    if ((b & probeStepMask) == 0) return;

    // Fail-safe encoding (§3.3): the dir bit is set only when the switch is
    // positively read as CLOSED. A corrupted byte, a dead node and bus silence
    // then all decode to "open — stop", so the safe state is the one that needs
    // no successful delivery. The switch is NC to GND against the internal
    // pull-up, so closed reads LOW.
    //
    // No debounce, no filtering, one read (§4.4). The first open is the true
    // surface, and any filter here would spend depth on every probe to reject
    // noise that §5.9 rejects for free once Z has stopped.
    uint8_t reply = digitalReadFast(HAL_VACUUM_SWITCH_PIN) ? 0x00 : probeDirMask;

    // Every step bit is clear: this byte is a report, never motion. A node that
    // answered with a step bit set would drive whatever stepper holds that slot.

    // Drop rather than answer late (§4.2). Under lockstep the Pico emits nothing
    // until this reply lands, so a byte already waiting in RXDATA means we did
    // not get here in time to be the answer to it — the Pico has moved on, and a
    // reply now would collide with whatever it is doing instead. Silence costs
    // one poll; a late byte costs a collision.
    //
    // CAVEAT: this catches the node falling a whole byte behind, which is the
    // only lateness the node can actually observe. It cannot detect a merely
    // delayed ISR entry, because the byte's arrival time is not recorded
    // anywhere the ISR can read. That residue is covered on the Pico's side, by
    // the per-leg deadline (§5.7) and by the encoding above making a missing
    // reply safe.
    if (HAL_USART_INST.STATUS & USART_RXCIF_bm) return;

    HAL_RS485_TX_BEGIN();
    HAL_USART_INST.STATUS  = USART_TXCIF_bm;
    while (!(HAL_USART_INST.STATUS & USART_DREIF_bm));
    HAL_USART_INST.TXDATAH = 0x00;      // 9th bit = 0 → stream byte
    HAL_USART_INST.TXDATAL = reply;

    // Hold the ISR until the byte has physically left, ~13 bit times (14 us at
    // 921600). On the ATtiny board this is mandatory: DE is a software GPIO and
    // releasing it early truncates the byte on the wire. On the AVR128DB32 the
    // USART drives DE itself through XDIR and the wait is not needed for
    // correctness — it is kept so both boards spend the same time in the ISR,
    // because §8.1 measures them against each other.
    while (!(HAL_USART_INST.STATUS & USART_TXCIF_bm));
    HAL_USART_INST.STATUS = USART_TXCIF_bm;
    HAL_RS485_TX_END();
}
