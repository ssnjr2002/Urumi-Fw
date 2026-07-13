#pragma once
#include <stdint.h>

// Core 0 data plane — binary MicroSegment / jog packet ingest.
// Framing and duplicate-guard semantics: docs/wire_protocol.md.

// Try to consume one USB byte as data-plane (binary) input. Returns true if the
// byte was taken — either we are mid-packet, or the byte is a data-plane magic
// starting a new packet. Returns false if it belongs to the control plane (a
// text-line byte), which the caller then handles.
bool dataPlaneConsume(uint8_t b);

// Time-based housekeeping — call once per Core 0 loop pass. Aborts a CFG_SET
// transfer that has stalled past CFG_RX_TIMEOUT_MS (byte-driven receive cannot
// self-timeout).
void dataPlaneTick();

// Soft-reset wipe: clear all ingest state (mid-packet buffer + duplicate-guard
// seq / ACK echo counters).
void dataPlaneReset();

// `seqreset` control command: zero the duplicate-guard seq and ACK echo only.
void dataPlaneResetSeq();
