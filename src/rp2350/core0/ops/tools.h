#pragma once
#include <stdint.h>

// What each head has mounted (docs/plans/motion-sessions.md, JB2): a config
// tool id, or 0 for none. RAM only; kept across jobs and soft resets, cleared
// at boot and when a new config is adopted.

uint8_t toolsMounted(uint8_t head);

// Mount `id` on `head`. Returns nullptr, or the refusal text after `err `:
// `no_head` (head out of range), `no_tool` (id not in the config).
const char* toolsMount(uint8_t head, uint8_t id);

void toolsReset(void);
