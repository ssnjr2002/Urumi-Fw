#pragma once

// The `jogto` sequence (docs/plans/motion-sessions.md, J1b): step 1 parks every
// present head's Z (LEGS), step 2 runs the anchor in a straight line to the
// target (RING, owner RING_SEQ), at the slower of X's and Y's jogFeed. Published
// as JOGGING with reason GOTO throughout; a failed park is F_HOMING; `stop`
// ends it as any sequence.

// Start a run to anchor machine mm (mx, my). X, Y and every present head's Z
// must be homed, and the Z nodes answer before anything moves. Returns nullptr
// once started, or the refusal text after `err ` with nothing moved. Gating the
// machine state is the caller's. `aLock`: the selected A rides the line to
// unlock (machine 0), as steps.h lineTo.
const char* jogtoStart(float mx, float my, bool aLock);
