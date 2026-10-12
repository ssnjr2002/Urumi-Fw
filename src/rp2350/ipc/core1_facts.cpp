#include "core1_facts.h"

#ifdef ARDUINO
#include "hardware/sync.h"
#else
#define __dmb() __sync_synchronize()
#define __time_critical_func(f) f
#endif

#define FACT_SLOTS 8u   // a power of two: the u8 indices wrap onto it

static volatile uint8_t ring[FACT_SLOTS];
static volatile uint8_t head;   // pushed so far; written by Core 1 only
static volatile uint8_t tail;   // popped so far; written by Core 0 only
static volatile bool    lost;   // set by Core 1, cleared by Core 0

// In RAM: the follower's slot loop pushes.
bool __time_critical_func(core1FactPush)(Core1Fact f) {
    const uint8_t h = head;
    if ((uint8_t)(h - tail) == FACT_SLOTS) {
        lost = true;
        return false;
    }
    ring[h % FACT_SLOTS] = f;
    __dmb();   // the slot before the head that publishes it
    head = h + 1;
    return true;
}

Core1Fact core1FactPop(void) {
    const uint8_t t = tail;
    if (t == head) return FACT_NONE;
    __dmb();   // the head before the slot it published
    const Core1Fact f = (Core1Fact)ring[t % FACT_SLOTS];
    __dmb();   // the slot read before the tail frees it
    tail = t + 1;
    return f;
}

bool core1FactsLost(void)      { return lost; }
void core1FactsLostClear(void) { lost = false; }

void core1FactsReset(void) {
    head = tail = 0;
    lost = false;
}
