#pragma once
#include <Arduino.h>

// Core 0 control plane — text command line handling (docs/wire_protocol.md).

// Handle one control-plane text line. Replies with exactly one line per the wire
// contract: `ok` / `err <reason>` / a typed read. Returns false if the command
// is unknown (caller prints `err unknown`).
bool handleCommand(const String& input);

// Reset the committed axis map to all-unbound (SLOT_NONE). Called by the Core 0
// soft-reset so each connect starts in the ALARM_CONFIG gate until the host
// (re-)commits an axis_map. See docs/engage_and_axis_map.md §6.
void axisMapReset();

// Fold Core 1's ALARM signals into the validity masks. Core 0 is the only writer
// of axes_homed / axes_enabled / nodeHomed; call this from the Core 0 loop before
// any host-observable reply is produced.
void reconcileValidity();
