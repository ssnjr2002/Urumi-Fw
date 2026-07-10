#pragma once
#include <Arduino.h>

// Core 0 control plane — text command line handling (docs/wire_protocol.md).

// Handle one control-plane text line. Replies with exactly one line per the wire
// contract: `ok` / `err <reason>` / a typed read. Returns false if the command
// is unknown (caller prints `err unknown`).
bool handleCommand(const String& input);
