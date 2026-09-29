#pragma once
#include <stdint.h>

// cycles.h — the CPU cycle counter, as rp2040.getCycleCount() but read straight
// from the core's DWT CYCCNT: one register load, always inlined, and it counts
// with interrupts off (getCycleCount extends SysTick from an interrupt). Wraps
// every 2³² cycles like getCycleCount, so `now - t0` works across the wrap.
//
// The DWT is per core: call cyclesInit() once on each core that reads it.

#define CYCLES_DEMCR    (*(volatile uint32_t*)0xE000EDFCu)
#define CYCLES_DWT_CTRL (*(volatile uint32_t*)0xE0001000u)
#define CYCLES_CYCCNT   (*(volatile uint32_t*)0xE0001004u)

static inline void cyclesInit() {
    CYCLES_DEMCR    |= 1u << 24;   // TRCENA: enable the DWT
    CYCLES_DWT_CTRL |= 1u;         // CYCCNTENA
}

static inline __attribute__((always_inline)) uint32_t cycleCount() {
    return CYCLES_CYCCNT;
}
