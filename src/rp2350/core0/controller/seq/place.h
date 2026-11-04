#pragma once
#include <stdint.h>
#include "../../ops/layer.h"

// A job's placement (docs/plans/motion-sessions.md, Placement). The last preflight
// TOOL_HDR swaps PREFLIGHT for PLACE; the tick then selects the pointer (the laser,
// else the head at the anchor), parks every Z and moves the sequence SEQ_PLACE to
// the handoff: the origin it picks plus the job's (xmin, ymin). XY jogs move the
// dot inside the valid origins until `place set` makes it the job's corner.
// `place ok` leaves for BUFFERING. Leaving any other way turns the laser off and,
// once the job has closed, gives back the selection.

// `x` and `y` are the valid origins; `box` xmin, xmax, ymin, ymax over the job's tools.
void placeBegin(const OriginRange& x, const OriginRange& y, const float box[4]);

// The Core 0 loop, after sequenceTick.
void placeTick(void);

// The `place` commands, each in JOB.PLACE with nothing moving. nullptr, or the refusal.
const char* placeSet(void);
const char* placeReset(void);
const char* placeGo(bool xmax, bool ymax);
const char* placeFrame(void);
const char* placeOk(void);
