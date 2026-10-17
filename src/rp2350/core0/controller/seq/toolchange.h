#pragma once
#include <stdint.h>

// A job's tool change (docs/plans/motion-sessions.md, JB4). A LAYER_HDR whose
// tool is not mounted on its head, or whose head is not selected, is NACKed
// (NACK_TOOL_CHANGE) and holds the job; once the ring is dry the job is in
// TOOL_CHANGE:
//   * the tool mounted on its head: select the head, then back;
//   * otherwise the sequence SEQ_TOOL_CHANGE: select the head, park every Z
//     (LEGS), a line to the head's probe switch with A to the holder's lock
//     (RING). Then TOOL_WAIT, the
//     head's work Z cleared: `mount`, `jog` and `wzero` are admitted, and the
//     tool mounted on the head with its Z zeroed goes back.
// Back is BUFFERING, or PAUSED if the change came from there; the host resends
// the header. A refusal after the NACK fails the change (sequenceFail), which
// ends the job.

// The header wants `tool` on `head`. False if the job cannot hold for it now.
bool toolChangeBegin(uint8_t head, uint8_t tool);

// The job is holding for a change: headers and records are NACK_TOOL_CHANGE.
bool toolChangeHolding(void);

// `mount <head> <id>`: in TOOL_WAIT only the wanted tool on the wanted head.
// Returns nullptr, or the refusal text.
const char* toolChangeMountCheck(uint8_t head, uint8_t id);

// From the Core 0 loop, after sequenceTick.
void toolChangeTick(void);
