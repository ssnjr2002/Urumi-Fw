#pragma once
#include <stdint.h>

// mesh.h — the bed mesh (docs/plans/planner-za.md, Decisions, Mesh).
//
// /mesh.bin is read into RAM at boot. While the mesh is active (loaded, on,
// X, Y and Z homed) actual Z = flat Z + offset(tip), the offset being the
// height at the tip less the height at the work origin. The planner's Z is
// flat: Core 0 latches the offset's inputs whenever the ring starts from the
// motors, and Core 1 adds the offset on the way to the followers.
//
// File, little-endian:
//   u32 magic 'MESH', u16 version, u16 nx, u16 ny, u16 pad,
//   f32 x0, y0, dx, dy (mm, tip machine coordinates),
//   i16 z[nx*ny] (µm, + up, x fastest), u32 crc32 of everything before it.
//
// Core-0-only, except the latch Core 1 reads (ipc/shared_state.h).

#define MESH_MAX_POINTS 16384

enum MeshFile : uint8_t { MESH_FILE_ABSENT, MESH_FILE_BAD, MESH_FILE_OK };

// Read /mesh.bin; after configStoreInit, which mounts LittleFS.
void meshInit(void);

MeshFile meshFile(void);
uint16_t meshNx(void);
uint16_t meshNy(void);
// The steepest slope between neighbouring points, mm/mm: Z's maxFeed over it
// is the slowest XY speed the mesh can force.
float meshSlope(void);

// `mesh on|off`: volatile, on at boot; takes effect at the next ring start.
bool meshEnabled(void);
void meshEnable(bool on);

// The offset at a tip position now, mm; 0 while the mesh is inactive.
float meshOffsetAt(float tipX, float tipY);

// Under plannerLock with the ring empty and Core 1 off it: copy the offset's
// inputs for this ring into plannerMesh.
void meshLatch(void);

// The latched offset at anchor position (x, y); 0 when the latch is off.
float meshLatchedOffset(float x, float y);
