#pragma once
#include <stdint.h>

// Core 0 status reporting.

// Planner blocks queued — shared by the binary STATUS_RSP and `get buf`.
uint16_t getBufCount();

// Binary mirror of the `get` state keys (docs/wire_protocol.md STATUS_REQ/STATUS_RSP).
// Accepted in every machine state; handled inline (no ring-buffer / Core 1
// interaction) so it never delays step timing.
void sendStatusRsp();
