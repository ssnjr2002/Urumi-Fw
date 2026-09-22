#pragma once
// motion_limits.h — TEMPORARY per-axis motion limits for the soft-abort ramp.
//
// Split out of shared.h: Core 0 has no use for these. DECEL_SPS2_* has no direct
// users at all -- it is consumed only by decelForAxis() below.
//
// This whole file is scheduled for deletion. The constants belong in the config
// blob alongside the accel limits they are derived from; they are #defines only
// because Core 1 has no config-read path yet -- the same gap that keeps
// rampStepInBounds() a stub. Fix both together and delete this file.

// Velocity at or below which a stop needs no ramp — start/stop speed.
#define V_REST_SPS   50.0f

// TEMPORARY — per-axis decel rate for the soft-abort ramp, steps/s².
//
// These belong in the config blob alongside the accel limits they are derived
// from, not in a header. They are #defines only because Core 1 has no
// config-read path yet — the same gap that keeps rampStepInBounds() a stub.
// Fix both together and delete this block.
//
// Seeded from web/demo/config.json as maxAccel (mm/s²) × stepsPerUnit
// (steps/mm), which is the same conversion the host planner does:
//   X  1000 × 160    = 160000
//   Y  1000 × 160    = 160000
//   A   500 ×  45.46 =  22730
// Z has NO maxAccel in that config — 150000 is a placeholder chosen to be
// unremarkable next to X/Y, not a measured limit. Treat it as unverified.
//
// Note the spread: stopping distance is v²/2a, so at 160000 steps/s² a
// 20 kHz move stops in ~1250 steps while the A axis takes ~8800. One global
// value could not have served both, which is the concrete argument for these
// being per-axis config rather than a constant.
#define DECEL_SPS2_X  160000.0f
#define DECEL_SPS2_Y  160000.0f
#define DECEL_SPS2_Z  150000.0f   // placeholder — no maxAccel in config
#define DECEL_SPS2_A   22730.0f

// A zero or negative rate makes the ramp loop non-terminating. Keep an
// equivalent runtime guard when these move into config.
static_assert(DECEL_SPS2_X > 0.0f && DECEL_SPS2_Y > 0.0f &&
              DECEL_SPS2_Z > 0.0f && DECEL_SPS2_A > 0.0f,
              "decel must be positive or the ramp never ends");

// The ramp paces the MAJOR axis — that is the axis `interval` describes, and the
// one the Bresenham accumulators are measured against — so the rate is selected
// by major-axis index, not by whichever axis is most constrained.
static inline float decelForAxis(int axis) {
    switch (axis) {
        case 0:  return DECEL_SPS2_X;
        case 1:  return DECEL_SPS2_Y;
        case 2:  return DECEL_SPS2_Z;
        default: return DECEL_SPS2_A;
    }
}
