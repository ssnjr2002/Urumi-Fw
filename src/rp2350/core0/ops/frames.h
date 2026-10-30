#pragma once
#include <stdint.h>

struct CfgAxis;

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

// The selected tip's XY offset from the anchor: the head's offset plus its
// mounted tool's. False with no selection.
bool framesTipOffset(float* dx, float* dy);

// The selected head's mounted blade: the tip's trail `*d` mm behind the knife
// axis and the edge's heading at machine A = 0. d is 0 without one.
void framesBlade(float* d, float* aOffsetDeg);

// Machine XY of the blade tip: MPos less the knife axis's lead at the
// current A. False as framesMPos.
bool framesTipMPos(float* x, float* y);

// Slot k's soft range for the blade tip, units: the axis's range, shrunk by
// the blade's d on X and Y. Z's floor rises to maxOvercut below the bed
// while the bound head has a work Z, `off` the mesh offset where Z is.
// False when configAxisRange is.
bool framesTipRange(uint8_t k, float* lo, float* hi, float off = 0);

// The head whose Z node is bound in slot Z, or FRAMES_NONE: Z, A and the
// per-head work Z belong to it.
uint8_t framesZHead(void);

// Machine position of slot k, units. False when slot k has no axis config
// (an unbound Z/A).
bool framesMPos(uint8_t k, float* out);

// Work position of slot k, units: tip − work offset on X/Y/Z (the blade tip), Z less the bed
// mesh; on A, the edge heading (MPos + the mounted tool's aOffsetDeg) folded
// to (−180, 180]. False as framesMPos, X/Y with no selection, or Z with
// no work Z.
bool framesWPos(uint8_t k, float* out);

// The bed mesh's offset at the selected tip now, mm (0 while inactive).
float framesMeshOffset(void);

// The config of the axis on slot k: X, Y, or the head bound to Z or A. Null
// without a config or a head on that slot.
const CfgAxis* framesAxis(uint8_t k);

// Work offset, machine units. Z is the Z head's.
float framesWork(uint8_t k);
// Set the work offset of slot k (0..2) in machine units. Z needs a Z head.
bool framesSetWork(uint8_t k, float v);
// XY back to the config's `work` block; every head's Z unset.
void framesClearWork(void);
// Clear head h's work Z for a new tool: 0 and unset until the next Z zero
// (framesSetWork). framesWorkZSet reports it.
void framesUnsetWorkZ(uint8_t h);
bool framesWorkZSet(uint8_t h);

// A work-coordinate XY target to the anchor's machine coordinates. Returns
// nullptr, or "no_head".
const char* framesToMachine(float wx, float wy, float* mx, float* my);

// A work target on slot Z or A to machine units. Z: the Z head's, plus the
// work Z and the bed mesh at the tip now; "no_work" until that head's Z is
// zeroed. A: the edge heading less the mounted tool's aOffsetDeg, on the turn
// nearest `from` (machine), within aMaxDeg of 0 for a cabled tool ("too_far"
// with none). nullptr, "no_head", "no_work" or "too_far".
const char* framesWorkToMachine(uint8_t k, float w, float from, float* m);

// A named position to the anchor's machine XY: `park` (where homing parks X
// and Y without one), `load` or `mount` (a tool change's stop). Returns nullptr, "usage" for another name, or
// "no_position" for one the config lacks.
const char* framesNamed(const char* name, float* mx, float* my);

// The anchor's machine XY that puts tool `tool`'s tip, on the selected head, on
// that head's probe switch, mounted or not. nullptr, "no_head", "no_tool" or
// "no_position".
const char* framesProbeFor(uint8_t tool, float* mx, float* my);

// Node `node` at machine position `machineSteps` (wire frame) inside its
// axis's soft range, when that axis has softLimits. nullptr, or "soft_limit".
const char* framesCheckNode(uint8_t node, int32_t machineSteps);

// A move of slot k (X, Y or Z) from `from` to `to`, machine mm, inside the soft
// range (framesTipRange) when that axis is homed with softLimits; from outside
// it, one that holds or heads in also passes. nullptr, or "soft_limit x|y|z"
// with `*left` the mm still free in the move's direction (never negative).
// `off` is Z's mesh offset, as framesTipRange. A has no soft range.
const char* framesCheckMove(uint8_t k, float from, float to, float* left, float off = 0);

// Machine XY inside the soft range on every homed X/Y axis with softLimits.
// Returns nullptr, or "soft_limit x|y".
const char* framesCheckXY(float mx, float my);
