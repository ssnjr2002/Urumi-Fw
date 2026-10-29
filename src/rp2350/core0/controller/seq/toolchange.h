#pragma once
#include <stdint.h>

// A job's tool change (docs/plans/motion-sessions.md, JB4 and JT). A LAYER_HDR
// whose tool is not mounted on its head, or not zeroed (no work Z set), or
// whose head is not selected while the bus is busy, is NACKed
// (NACK_TOOL_CHANGE) and holds the job; once the ring is dry the job is in
// TOOL_CHANGE:
//   * the tool mounted and zeroed: select the head, then back;
//   * otherwise the sequence SEQ_TOOL_CHANGE, the stop: select the head and
//     unmount it (tool 0), park every Z, turn its A to machine 0 if it has
//     `unlockRange` (LEGS), a line to the mount position (RING). Then
//     TOOL_WAIT: `mount`, `unmount` (any head), `jog`, `wzero` and
//     `probe_park` are admitted.
// Only a resent header leaves TOOL_WAIT: with its tool mounted on its head and
// zeroed it goes back and is taken; otherwise it is NACK_TOOL_CHANGE again.
// Back is BUFFERING, or PAUSED if the change came from there. A refusal after
// the NACK fails the change (sequenceFail), which ends the job.

// The header wants `tool` on `head`. False if the job cannot hold for it now.
bool toolChangeBegin(uint8_t head, uint8_t tool);

// The job is holding for a change: records, and headers outside TOOL_WAIT, are
// NACK_TOOL_CHANGE.
bool toolChangeHolding(void);

// The job is in TOOL_WAIT.
bool toolChangeWaiting(void);

// A header in TOOL_WAIT wants `tool` on `head`: with it mounted and zeroed and
// nothing moving, select the head and go back. False to NACK it.
bool toolChangeLeave(uint8_t head, uint8_t tool);

// From the Core 0 loop, after sequenceTick.
void toolChangeTick(void);
