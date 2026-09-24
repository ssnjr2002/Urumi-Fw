#pragma once

// state.h — settle the machine state from the conditions that still hold.

// Leave a completed leg (or a cleared alarm) in the right state: ALARM_CONFIG
// without a valid config, ALARM_NODE_FAULT while the axis map is incomplete,
// ALARM/LIMIT_LATCHED while any axis is still standing on a switch, IDLE once
// none of those hold.
//
// DERIVED, never assigned, and that is the whole point. Legs 1 and 3 end
// latched and legs 2 and 4 end clear, so a sequence that wrote the state
// directly at each leg would be correct only until two axes homed with
// interleaved legs -- then clearing one switch would report IDLE with the
// other still down. Recomputing from the mask cannot produce that.
void resumeOrHold(void);
