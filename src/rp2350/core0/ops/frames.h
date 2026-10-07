#pragma once
#include <stdint.h>

// frames.h — the controlled point, the work offset, MPos/WPos and the soft
// range (docs/plans/coordinate-system.md).
//
// Machine coordinates are units (mm, deg) with + physical: steps ÷
// stepsPerUnit, negated on an `invertDir` axis. The controlled point is the
// selected tip: the anchor plus the selected head's offset (zero for the
// anchor). The work offset is a volatile copy of the config's `work` block:
// XY shared, Z per head. Move targets arrive in work coordinates and leave
// here as anchor (machine) coordinates.
//
// Core-0-only; no bus I/O except the laser switch in framesSelect/framesOnMap.

#define FRAMES_NONE   0xFF   // no selection: the XY offset is unknown
#define FRAMES_ANCHOR 0xFE   // the laser tip; without a laser, the head at (0, 0)

// Config committed (or gone): work offset from the config, no selection
// until the next map commit picks one.
void framesReset(void);

// A head index, FRAMES_ANCHOR or FRAMES_NONE, derived from the axes request:
// the stored selection while it is mapped, else the anchor (laser, X and Y
// mapped), else none.
uint8_t framesSelected(void);

// Select the laser anchor: no map change, laser on. Only with a laser; the
// head form of `select` commits that head's map and framesOnMap picks it.
// False, laser untouched, while X or Y is unmapped.
bool framesSelectLaser(void);

// `select <head>` after committing that head's map: selects it (laser off)
// if the request binds it, even when the map did not change.
void framesSelectHead(uint8_t h);

// After every committed axes map: keep the selection if it still matches,
// else the head whose Z and A the map binds, else the laser anchor, else
// none. Switching to a head turns the laser off.
void framesOnMap(void);

// The selected tip's XY offset from the anchor. False with no selection.
bool framesTipOffset(float* dx, float* dy);

// The head whose Z node is bound in slot Z, or FRAMES_NONE: Z, A and the
// per-head work Z belong to it.
uint8_t framesZHead(void);

// Machine position of slot k, units. False when slot k has no axis config
// (an unbound Z/A).
bool framesMPos(uint8_t k, float* out);

// Work position of slot k, units: tip − work offset on X/Y/Z; on A, MPos
// folded to (−180, 180]. False as framesMPos, or X/Y with no selection.
bool framesWPos(uint8_t k, float* out);

// Work offset, machine units. Z is the Z head's.
float framesWork(uint8_t k);
// Set the work offset of slot k (0..2) in machine units. Z needs a Z head.
bool framesSetWork(uint8_t k, float v);
// Back to the config's `work` block.
void framesClearWork(void);

// A work-coordinate XY target to the anchor's machine coordinates. Returns
// nullptr, or "no_head".
const char* framesToMachine(float wx, float wy, float* mx, float* my);

// Node `node` at machine position `machineSteps` (wire frame) inside its
// axis's soft range, when that axis has softLimits. nullptr, or "soft_limit".
const char* framesCheckNode(uint8_t node, int32_t machineSteps);

// Machine XY inside the soft range on every homed X/Y axis with softLimits.
// Returns nullptr, or "soft_limit x|y".
const char* framesCheckXY(float mx, float my);
