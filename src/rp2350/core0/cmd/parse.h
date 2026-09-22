#pragma once
#include <stdint.h>

// parse.h — argument parsing shared by the command handlers.
//
// Handlers receive `args` already advanced past the command word and any spaces
// (table.h), so nothing here needs to know a command's name or length. That is
// the point: the old chain hand-maintained `argAfter(input, 11)` at twenty-odd
// sites, each of which had to equal strlen of its own command word, and each a
// silent bug on rename.

// Parse an on/off token: "1" or "on" → true; anything else ("0"/"off") → false.
bool parseState(const char* s);

// Map an axes string ("xyza", "xy", …) to a bitmask. Empty/absent → all axes.
uint8_t axisMask(const char* s);

// Parse a bus id from `p`, advancing `*end` past it. Returns 0 -- never a valid
// id -- if the token is absent, malformed, or out of the 1..BUS_ADDR_MAX range.
//
// Callers still print their own error, because the string is not uniform across
// commands: some say `err usage`, some `err bad_node`. Those strings are on the
// wire and mirrored in host/protocol/link.py, so normalising them is its own
// change with the host in step -- not a side effect of this one.
uint8_t parseNode(const char* p, char** end);
