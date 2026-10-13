#pragma once

// Native stand-in for the Pico SDK's spin locks and barriers: the tests are
// single-threaded.

#include <stdint.h>

typedef struct { uint32_t unused; } spin_lock_t;

static inline uint32_t spin_lock_blocking(spin_lock_t*) { return 0; }
static inline void     spin_unlock(spin_lock_t*, uint32_t) {}
static inline void     __dmb(void) {}
