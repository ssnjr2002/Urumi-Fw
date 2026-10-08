// mesh.cpp — /mesh.bin into RAM, and the offset rule (mesh.h).

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include "mesh.h"
#include "frames.h"
#include "position.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"
#include <planner/mesh.h>

static const char* const kPath = "/mesh.bin";
static constexpr uint32_t kMagic   = 0x4853454Du;   // "MESH"
static constexpr uint16_t kVersion = 1;

struct __attribute__((packed)) MeshHeader {
    uint32_t magic;
    uint16_t version, nx, ny, pad;
    float    x0, y0, dx, dy;
};

static int16_t        heights[MESH_MAX_POINTS];
static planner::Mesh  grid;
static MeshFile       file = MESH_FILE_ABSENT;
static bool           enabled = false;
static float          slopeMax = 0;

static uint32_t crc32Fold(uint32_t crc, const uint8_t* p, uint32_t n) {
    while (n--) {
        crc ^= *p++;
        for (uint8_t k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc;
}

static MeshFile load() {
    if (!LittleFS.exists(kPath)) return MESH_FILE_ABSENT;
    File f = LittleFS.open(kPath, "r");
    if (!f) return MESH_FILE_BAD;
    MeshHeader h;
    bool ok = f.read((uint8_t*)&h, sizeof(h)) == sizeof(h)
           && h.magic == kMagic && h.version == kVersion
           && (uint32_t)h.nx * h.ny <= MESH_MAX_POINTS
           && f.size() == sizeof(h) + 2u * h.nx * h.ny + 4u;
    uint32_t crc = crc32Fold(0xFFFFFFFFu, (const uint8_t*)&h, sizeof(h));
    const uint32_t bytes = ok ? 2u * h.nx * h.ny : 0;
    ok = ok && f.read((uint8_t*)heights, bytes) == bytes;
    uint32_t want = 0;
    ok = ok && f.read((uint8_t*)&want, 4) == 4;
    f.close();
    if (!ok) return MESH_FILE_BAD;
    crc = ~crc32Fold(crc, (const uint8_t*)heights, bytes);
    if (crc != want) return MESH_FILE_BAD;
    if (!grid.set(h.nx, h.ny, h.x0, h.y0, h.dx, h.dy, heights)) return MESH_FILE_BAD;
    slopeMax = 0;
    for (int j = 0; j < h.ny; j++)
        for (int i = 0; i < h.nx; i++) {
            const int16_t* p = heights + j * h.nx + i;
            if (i + 1 < h.nx) slopeMax = fmaxf(slopeMax, abs(p[1] - p[0]) * 0.001f / h.dx);
            if (j + 1 < h.ny) slopeMax = fmaxf(slopeMax, abs(p[h.nx] - p[0]) * 0.001f / h.dy);
        }
    return MESH_FILE_OK;
}

void meshInit(void) {
    grid = planner::Mesh();
    file = load();
    if (file != MESH_FILE_OK) grid = planner::Mesh();
    enabled = machineCfgValid() && machineCfg().meshOn;
}

MeshFile meshFile(void) { return file; }
uint16_t meshNx(void) { return file == MESH_FILE_OK ? grid.nx : 0; }
uint16_t meshNy(void) { return file == MESH_FILE_OK ? grid.ny : 0; }
float meshSlope(void) { return file == MESH_FILE_OK ? slopeMax : 0; }

bool meshEnabled(void) { return enabled; }
void meshEnable(bool on) { enabled = on; }

static bool active(void) {
    const uint8_t xyz = (1u << SLOT_X) | (1u << SLOT_Y) | (1u << SLOT_Z);
    return file == MESH_FILE_OK && enabled && (axes_homed & xyz) == xyz;
}

// The height at the work origin: the work offset's XY is a tip position.
static float reference(void) { return planner::meshAt(grid, framesWork(SLOT_X), framesWork(SLOT_Y)); }

float meshOffsetAt(float tipX, float tipY) {
    if (!active()) return 0;
    return planner::meshAt(grid, tipX, tipY) - reference();
}

void meshLatch(void) {
    float dx = 0, dy = 0;
    const bool on = active() && framesTipOffset(&dx, &dy);
    plannerMesh.tipX = dx;
    plannerMesh.tipY = dy;
    plannerMesh.ref  = on ? reference() : 0;
    plannerMesh.mesh = on ? &grid : nullptr;
}

float meshLatchedOffset(float x, float y) {
    const planner::Mesh* m = plannerMesh.mesh;
    if (!m) return 0;
    return planner::meshAt(*m, x + plannerMesh.tipX, y + plannerMesh.tipY) - plannerMesh.ref;
}
