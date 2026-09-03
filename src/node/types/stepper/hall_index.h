#pragma once
#include <stdint.h>
#include "board.h"
#include "stepper/stepper.h"

#ifdef HAS_HALL_INDEX

// hall_index — rotary index finding, ported from src/scratch/hall_capture.cpp.
//
// The estimator (est_mirror) and every constant here were chosen against real
// captures on node 4 and are documented in docs/rotary_a_axis.md §2. Nothing in
// this module re-litigates that; it is the same arithmetic under node
// constraints, driven by the homing pulser instead of a scratch step loop.
//
// THE STRUCTURAL DIFFERENCE FROM A LIMIT SWITCH, which shapes this whole API:
// a switch edge IS the position, so a linear seek can stop the instant it
// detects. An analog dip's centre is only knowable after passing it, so the
// sweep must run through the whole feature, buffer it, and reduce afterwards.
// Hence three calls rather than one:
//
//   hallIndexArm()      loop context, at the arm. Resets the state machine.
//   hallIndexSample()   ISR, once per step, after the step. Cheap: one ADC
//                       register read and a decimated state machine. Returns
//                       true when a COMPLETE dip has passed and the pulser
//                       should stop.
//   hallIndexResolve()  loop context, after the pulser stops. Runs the O(n^2)
//                       autoconvolution (~50 ms) and produces the answer.
//
// The split is forced by cost, not taste: the reduction is ~800k cycles against
// a step budget of a few microseconds, so it cannot live in the ISR.
//
// It does not run to completion in loop context either. A node dispatches RS485
// commands FROM loop() — the RX ISR only fills the queue — so a resolve that
// blocked for its full O(n^2) runtime (~170 ms at HOME_WIN on a 24 MHz AVR)
// answered nothing for that whole time, and the supervisor failed every
// successful home as HOMEFAIL_POLL. Hence Begin/Step below.

// Called once from node_setup(). Puts the ADC in free-running mode; see the
// .cpp for why the conversion is not started per-step.
void hallIndexSetup(void);

// Reset for a new sweep. `sign` is what one pulser step adds to
// absolutePosition (+1 or -1), so the window can be mapped back to absolute
// step coordinates.
void hallIndexArm(int8_t sign, int32_t posNow);

// One step's worth of work, from the pulser ISR. Returns true when the dip is
// complete and the move should end.
bool hallIndexSample(int32_t posNow);

// Reduce the buffered window, across several loop() passes.
//
// Begin() once after the pulser stops: it does everything cheap, and settles
// every refusal that does not need the correlation. Step() then runs one
// bounded slice per call and returns true when the index and cause are final —
// so the caller must keep NODE_FLAG_HOMING set until it does. "Still homing" is
// the correct reading while the answer does not yet exist; the alternative is a
// master that reads a stale cause from the previous sweep.
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

// Crossings completed. THE diagnostic when the answer is NOTFOUND: 0 says the
// magnet was never seen at all (no sensor, no magnet, or no rotation), and
// anything between 1 and the required count says the budget ran out before the
// sweep could prove the feature repeats. Those two were indistinguishable
// before, and telling them apart cost three bench sessions.
uint8_t hallIndexCrossings(void);

// Bring-up readouts: the live sensor value, and the away-from-magnet baseline
// the last sweep judged its thresholds against.
int16_t hallIndexRaw(void);
int16_t hallIndexBaseline(void);

#endif  // HAS_HALL_INDEX
