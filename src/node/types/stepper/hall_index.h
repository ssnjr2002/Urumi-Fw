#pragma once
#include <stdint.h>
#include "board.h"
#include "stepper/stepper.h"

#ifdef HAS_HALL_INDEX

// hall_index — rotary index finding (docs/system/nodes.md §9). The estimator
// and its constants come from captures documented in docs/rotary_a_axis.md.
//
// A dip's centre is known only after passing it, so a sweep runs through the
// whole dip, keeps its samples and resolves afterwards:
//
//   hallIndexArm()           loop context, at the arm.
//   hallIndexSample()        pulser ISR, after each step; true when the sweep
//                            should stop.
//   hallIndexResolveBegin()  loop context, once the pulser stops.
//   hallIndexResolveStep()   loop context, one slice per pass, so the node keeps
//                            answering the bus during the ~170 ms correlation.

// Called once from node_setup(). Puts the ADC in free-running mode.
void hallIndexSetup(void);

// Reset for a new sweep. `sign` is what one pulser step adds to
// absolutePosition (+1 or -1), so the window can be mapped back to absolute
// step coordinates.
void hallIndexArm(int8_t sign, int32_t posNow);

// One step's worth of work, from the pulser ISR. Returns true when the dip is
// complete and the move should end.
bool hallIndexSample(int32_t posNow);

// Begin() settles every case that needs no correlation. Step() runs one slice
// and returns true when the index and cause are final; the caller keeps
// NODE_FLAG_LEG set until then.
void hallIndexResolveBegin(void);
bool hallIndexResolveStep(void);

// Last completed sweep's answer, for the status tail. `index` is only
// meaningful when cause == ROTARY_IDX_OK.
int32_t hallIndexPos(void);
uint8_t hallIndexCause(void);

// Steps per revolution, MEASURED as the mean interval between consecutive index
// crossings. Not knowable in advance on an unknown head, and the quantity every
// step-domain decision downstream depends on. Valid once cause is OK or SLIP.
int32_t hallIndexStepsPerRev(void);

// Worst disagreement between those intervals. Small is belt compliance; large
// means the axis did not travel what it was told, which is what SLIP reports.
int32_t hallIndexLapSpread(void);

// Crossings completed. For NOTFOUND, 0 means the magnet was never seen, and
// fewer than required that the budget ran out first.
uint8_t hallIndexCrossings(void);

// Bring-up readouts: the live sensor value, and the away-from-magnet baseline
// the last sweep judged its thresholds against.
int16_t hallIndexRaw(void);
int16_t hallIndexBaseline(void);

#endif  // HAS_HALL_INDEX
