#pragma once
#include <stdint.h>

// Core 0 status reporting.

// Ring-buffer occupancy (MicroSegments queued, tail-head wrapped) — telemetry
// shared by the binary STATUS_RSP and the human-readable text `status` alias.
uint16_t getBufCount();

// Binary mirror of `getstate` (docs/wire_protocol.md STATUS_REQ/STATUS_RSP).
// Accepted in every machine state; handled inline (no ring-buffer / Core 1
// interaction) so it never delays step timing.
void sendStatusRsp();
