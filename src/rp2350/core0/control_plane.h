#pragma once
#include <Arduino.h>

// Core 0 control plane — text command dispatch (docs/wire_protocol.md).
// The handlers themselves live in cmd/; see cmd/table.h.

// Handle one control-plane text line. Replies with exactly one line per the wire
// contract: `ok` / `err <reason>` / a typed read. Returns false if the command
// is unknown (caller prints `err unknown`).
bool handleCommand(const String& input);
