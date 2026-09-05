#pragma once
#include <stdint.h>

// axis_map.h — the axis-map commit, exposed so the probe session can reach it.
//
// Split out of cmdAxisMap rather than duplicated. The probe's teardown has to
// put the committed map back (docs/tool_probe.md §5.5), and the one thing it
// must NOT do is restore from remembered state: this path is "deliberately dumb,
// not a diff" — it disengages everything and rebuilds machinePos, axes_homed and
// homingLatched out of the ENGAGE acks, so it is correct even if a node reset
// during the probe. A second binder restoring from saved fields is precisely
// where that would go wrong, and axis.cpp is the only file that writes the
// position model.
//
// `desired` is four entries: a bus id, or SLOT_NONE for an unbound slot.
// `quiet` suppresses the `ok` / `err …` line — the control plane owes exactly
// one reply per command, and when this runs as part of a probe teardown the
// probe command has already spoken (or is about to).
//
// Returns false if a node refused to engage. The map is left as far as it got:
// the same defined degradation any axis_map produces on a flaky bus.
bool axisMapApply(const uint8_t* desired, bool quiet);
