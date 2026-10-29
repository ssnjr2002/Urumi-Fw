#pragma once
#include <stdint.h>

// What each head has mounted (docs/plans/motion-sessions.md, JB2): a config
// tool id, or 0 for none. RAM only; kept across jobs and soft resets, cleared
// at boot and when a new config is adopted.

uint8_t toolsMounted(uint8_t head);

// Mount `id` on `head`. Returns nullptr, or the refusal text after `err `:
// `no_head` (head out of range), `no_tool` (id not in the config). A change of
// tool unsets the head's work Z (framesUnsetWorkZ) until it is zeroed again.
const char* toolsMount(uint8_t head, uint8_t id);

void toolsReset(void);

// The holders that can release now, bit h = head h (get unlocked=): IDLE or
// JOB.TOOL_WAIT, nothing moving, and the head's A homed with |machine A| within
// its unlockRange. A head without unlockRange is never set.
uint8_t toolsUnlocked(void);
