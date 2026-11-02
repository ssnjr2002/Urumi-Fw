// Generated from web/src/machine/runtime.ts by `pnpm gen:runtime`. Do not edit.
#pragma once
#include <math.h>

enum RtScope { RT_SCOPE_MACHINE, RT_SCOPE_AXIS, RT_SCOPE_HEAD, RT_SCOPE_TOOL };
enum RtType { RT_TYPE_F32, RT_TYPE_BOOL, RT_TYPE_U16, RT_TYPE_POINT };
enum RtHoming { RT_HOMING_NONE, RT_HOMING_LINEAR, RT_HOMING_ROTARY, RT_HOMING_BOTH };
enum RtEffect : unsigned {
    RT_FX_voidDatum = 1u << 0,
    RT_FX_applyMesh = 1u << 1,
    RT_FX_republishBuffer = 1u << 2,
    RT_FX_refreshFrames = 1u << 3,
    RT_FX_refreshTool = 1u << 4,
};

#define RT_NO_LIMIT NAN

// REQUIRED(scope, path, member, type, min, max, minExclusive, homing, effects)
// OPTIONAL(scope, path, member, has, type, min, max, minExclusive, homing, effects)
#define RUNTIME_FIELDS(REQUIRED, OPTIONAL) \
    OPTIONAL(RT_SCOPE_MACHINE, "positions.park", positions.park, positions.hasPark, RT_TYPE_POINT, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    OPTIONAL(RT_SCOPE_MACHINE, "positions.load", positions.load, positions.hasLoad, RT_TYPE_POINT, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    OPTIONAL(RT_SCOPE_MACHINE, "positions.mount", positions.mount, positions.hasMount, RT_TYPE_POINT, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "feed.cutFallback", feed.cutFallback, RT_TYPE_F32, 1.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "feed.travelDefault", feed.travelDefault, RT_TYPE_F32, 1.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "feed.plungeFallback", feed.plungeFallback, RT_TYPE_F32, 1.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "feed.liftFeed", feed.liftFeed, RT_TYPE_F32, 1.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "buffer.queueTime", buffer.queueTime, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_NONE, RT_FX_republishBuffer) \
    REQUIRED(RT_SCOPE_MACHINE, "buffer.idleTime", buffer.idleTime, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_NONE, RT_FX_republishBuffer) \
    REQUIRED(RT_SCOPE_MACHINE, "zClearance", zClearance, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "maxOvercut", maxOvercut, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_MACHINE, "meshOn", meshOn, RT_TYPE_BOOL, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_applyMesh) \
    REQUIRED(RT_SCOPE_AXIS, "stepsPerUnit", stepsPerUnit, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_NONE, RT_FX_voidDatum) \
    REQUIRED(RT_SCOPE_AXIS, "invertDir", invertDir, RT_TYPE_BOOL, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_voidDatum) \
    REQUIRED(RT_SCOPE_AXIS, "maxTravel", maxTravel, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_voidDatum) \
    REQUIRED(RT_SCOPE_AXIS, "maxFeed", maxFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_AXIS, "maxAccel", maxAccel, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_AXIS, "jogFeed", jogFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_AXIS, "jogFeedUnhomed", jogFeedUnhomed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.startFeed", homing.startFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_BOTH, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.rampSteps", homing.rampSteps, RT_TYPE_U16, 0.0f, 65535.0f, false, RT_HOMING_BOTH, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.seekFeed", homing.seekFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_LINEAR, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.latchFeed", homing.latchFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_LINEAR, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.seekScaler", homing.seekScaler, RT_TYPE_F32, 1.0f, RT_NO_LIMIT, false, RT_HOMING_LINEAR, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.backoffDist", homing.backoffDist, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_LINEAR, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.pullOffDist", homing.pullOffDist, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_LINEAR, RT_FX_voidDatum) \
    OPTIONAL(RT_SCOPE_AXIS, "homing.parkPos", homing.parkPos, homing.hasParkPos, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_LINEAR, RT_FX_voidDatum) \
    REQUIRED(RT_SCOPE_AXIS, "homing.sweepFeed", homing.sweepFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_ROTARY, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.budgetRevs", homing.budgetRevs, RT_TYPE_F32, 3.2f, RT_NO_LIMIT, false, RT_HOMING_ROTARY, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.toleranceDeg", homing.toleranceDeg, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, true, RT_HOMING_ROTARY, 0) \
    REQUIRED(RT_SCOPE_AXIS, "homing.indexPos", homing.indexPos, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_ROTARY, RT_FX_voidDatum) \
    REQUIRED(RT_SCOPE_HEAD, "xOffset", xOffset, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshFrames) \
    REQUIRED(RT_SCOPE_HEAD, "yOffset", yOffset, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshFrames) \
    OPTIONAL(RT_SCOPE_HEAD, "probeSwitch", probeSwitch, hasProbeSwitch, RT_TYPE_POINT, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    OPTIONAL(RT_SCOPE_HEAD, "probeSwitch.z", probeSwitch.z, probeSwitch.hasZ, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    OPTIONAL(RT_SCOPE_HEAD, "unlockRange", unlockRange, hasUnlockRange, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    OPTIONAL(RT_SCOPE_TOOL, "cutFeed", cutFeed, hasCutFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    OPTIONAL(RT_SCOPE_TOOL, "plungeFeed", plungeFeed, hasPlungeFeed, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, 0) \
    REQUIRED(RT_SCOPE_TOOL, "toolOffset.xOffset", toolOffset.xOffset, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshFrames) \
    REQUIRED(RT_SCOPE_TOOL, "toolOffset.yOffset", toolOffset.yOffset, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshFrames) \
    OPTIONAL(RT_SCOPE_TOOL, "offsetMm", offsetMm, hasOffsetMm, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshTool) \
    OPTIONAL(RT_SCOPE_TOOL, "aOffsetDeg", aOffsetDeg, hasAOffsetDeg, RT_TYPE_F32, RT_NO_LIMIT, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshTool) \
    OPTIONAL(RT_SCOPE_TOOL, "aMaxDeg", aMaxDeg, hasAMaxDeg, RT_TYPE_F32, 0.0f, RT_NO_LIMIT, false, RT_HOMING_NONE, RT_FX_refreshTool) \
    REQUIRED(RT_SCOPE_TOOL, "cornerAngleDeg", cornerAngleDeg, RT_TYPE_F32, 0.0f, 180.0f, false, RT_HOMING_NONE, RT_FX_refreshTool) \

