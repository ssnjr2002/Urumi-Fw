// frames.cpp — see frames.h.

#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "frames.h"
#include "mesh.h"
#include "position.h"
#include "tools.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"

static uint8_t selected = FRAMES_NONE;
static float   workX = 0, workY = 0, workZ[CFG_MAX_HEADS] = {0};
static bool    zSet[CFG_MAX_HEADS];   // false from a tool change until a Z zero

static void laserSet(bool on) {
    if (!machineCfgValid() || !machineCfg().laserNode) return;
    rpcNodeCmd(CMD_LASER, machineCfg().laserNode, on ? 1u : 0u);   // best effort
}

void framesClearWork(void) {
    if (!machineCfgValid()) return;
    const MachineCfg& c = machineCfg();
    workX = c.workX;
    workY = c.workY;
    for (uint8_t h = 0; h < c.headCount; h++) { workZ[h] = c.workZ[h]; zSet[h] = true; }
}

void framesUnsetWorkZ(uint8_t h) {
    if (h >= CFG_MAX_HEADS) return;
    workZ[h] = 0;
    zSet[h]  = false;
}

bool framesWorkZSet(uint8_t h) { return h < CFG_MAX_HEADS && zSet[h]; }

void framesReset(void) {
    selected = FRAMES_NONE;
    framesClearWork();
}

static bool xyMapped(void) {
    return axesReqAt(SLOT_X) != SLOT_NONE && axesReqAt(SLOT_Y) != SLOT_NONE;
}

// True when the axes request maps X, Y and head h's Z and A (a head with no
// Z or A matches SLOT_NONE there).
static bool headMapped(uint8_t h) {
    if (!xyMapped()) return false;
    uint8_t map[4];
    configSlotMap(machineCfg(), h, SLOT_NONE, map);
    return map[SLOT_Z] == axesReqAt(SLOT_Z) && map[SLOT_A] == axesReqAt(SLOT_A);
}

// The stored selection while the axes request still carries it: a head needs
// its Z and A mapped, the anchor X and Y. Otherwise the anchor when X and Y are
// mapped and there is a laser, else none.
uint8_t framesSelected(void) {
    if (!machineCfgValid()) return FRAMES_NONE;
    const MachineCfg& c = machineCfg();
    if (selected < c.headCount && headMapped(selected)) return selected;
    return (xyMapped() && c.laserNode) ? FRAMES_ANCHOR : FRAMES_NONE;
}

bool framesSelectLaser(void) {
    if (!xyMapped()) return false;
    selected = FRAMES_ANCHOR;
    laserSet(true);
    return true;
}

// The head whose node on slot k (Z or A) is `node`, or FRAMES_NONE.
static uint8_t headOwning(uint8_t k, uint8_t node) {
    if (node == SLOT_NONE || !machineCfgValid()) return FRAMES_NONE;
    const MachineCfg& c = machineCfg();
    for (uint8_t h = 0; h < c.headCount; h++) {
        const CfgNode& n = k == SLOT_Z ? c.heads[h].z.node : c.heads[h].a.node;
        if (n.present && n.id == node) return h;
    }
    return FRAMES_NONE;
}

void framesOnMap(void) {
    static uint8_t lastBound = FRAMES_NONE;
    if (!machineCfgValid()) { selected = lastBound = FRAMES_NONE; return; }
    const MachineCfg& c = machineCfg();

    // The head the request binds: Z and A both its own (an absent node is
    // SLOT_NONE on both sides).
    uint8_t bound = FRAMES_NONE;
    for (uint8_t h = 0; h < c.headCount && bound == FRAMES_NONE; h++)
        if (headMapped(h)) bound = h;
    const bool switched = bound != lastBound;
    lastBound = bound;

    // A map that leaves the head as it was keeps a laser selection: the laser
    // has no slot. A head switch selects that head and turns the laser off.
    if (!switched && selected == FRAMES_ANCHOR && c.laserNode) return;
    if (bound != FRAMES_NONE) {
        if (switched || selected != bound) laserSet(false);
        selected = bound;
    } else {
        selected = c.laserNode ? FRAMES_ANCHOR : FRAMES_NONE;
    }
}

void framesSelectHead(uint8_t h) {
    if (!machineCfgValid() || h >= machineCfg().headCount || !headMapped(h)) return;
    if (selected != h) laserSet(false);
    selected = h;
}

bool framesTipOffset(float* dx, float* dy) {
    const uint8_t sel = framesSelected();
    if (sel == FRAMES_NONE) return false;
    if (sel == FRAMES_ANCHOR) { *dx = 0; *dy = 0; return true; }
    const MachineCfg& c = machineCfg();
    const CfgHead& h = c.heads[sel];
    const CfgTool* t = configTool(c, toolsMounted(sel));
    *dx = h.xOffset + (t ? t->xOffset : 0);
    *dy = h.yOffset + (t ? t->yOffset : 0);
    return true;
}

void framesBlade(float* d, float* aOffsetDeg) {
    *d = 0;
    *aOffsetDeg = 0;
    const uint8_t sel = framesSelected();
    if (sel >= machineCfg().headCount) return;
    if (const CfgTool* t = configTool(machineCfg(), toolsMounted(sel))) {
        *d = t->offsetMm;
        *aOffsetDeg = t->aOffsetDeg;
    }
}

bool framesTipMPos(float* x, float* y) {
    if (!framesMPos(SLOT_X, x) || !framesMPos(SLOT_Y, y)) return false;
    PlannerBlade b;
    float a;
    framesBlade(&b.d, &b.aOffsetDeg);
    if (b.d == 0 || !framesMPos(SLOT_A, &a)) return true;
    const planner::Vec2 lead = bladeLead(b, a);
    *x -= lead.x;
    *y -= lead.y;
    return true;
}

bool framesTipRange(uint8_t k, float* lo, float* hi) {
    const CfgAxis* a = framesAxis(k);
    if (!a || !configAxisRange(*a, lo, hi)) return false;
    if (k > SLOT_Y) return true;
    float d, off;
    framesBlade(&d, &off);
    *lo += d;
    *hi -= d;
    return true;
}

uint8_t framesZHead(void) { return headOwning(SLOT_Z, axisNode(SLOT_Z)); }

const CfgAxis* framesAxis(uint8_t k) {
    if (!machineCfgValid()) return nullptr;
    const MachineCfg& c = machineCfg();
    if (k == SLOT_X) return &c.x;
    if (k == SLOT_Y) return &c.y;
    const uint8_t h = headOwning(k, axisNode(k));
    if (h == FRAMES_NONE) return nullptr;
    return k == SLOT_Z ? &c.heads[h].z : &c.heads[h].a;
}

bool framesMPos(uint8_t k, float* out) {
    const CfgAxis* a = framesAxis(k);
    if (!a) return false;
    const float u = (float)machinePos[k] / a->stepsPerUnit;
    *out = (a->invertDir ? -u : u) + 0.0f;   // no -0

    return true;
}

float framesWork(uint8_t k) {
    if (k == SLOT_X) return workX;
    if (k == SLOT_Y) return workY;
    const uint8_t h = framesZHead();
    return (k == SLOT_Z && h != FRAMES_NONE) ? workZ[h] : 0;
}

bool framesSetWork(uint8_t k, float v) {
    if (k == SLOT_X) { workX = v; return true; }
    if (k == SLOT_Y) { workY = v; return true; }
    const uint8_t h = framesZHead();
    if (k != SLOT_Z || h == FRAMES_NONE) return false;
    workZ[h] = v;
    zSet[h]  = true;
    return true;
}

bool framesWPos(uint8_t k, float* out) {
    float m;
    if (!framesMPos(k, &m)) return false;
    if (k == SLOT_A) {
        // The edge heading: the mounted tool's offset from the lock.
        const uint8_t sel = framesSelected();
        if (sel < machineCfg().headCount) {
            const CfgTool* t = configTool(machineCfg(), toolsMounted(sel));
            if (t) m += t->aOffsetDeg;
        }
        const float f = fmodf(fmodf(m, 360.0f) + 360.0f, 360.0f);
        *out = f > 180.0f ? f - 360.0f : f;
        return true;
    }
    float dx = 0, dy = 0, tx, ty;
    if (k != SLOT_Z && !framesTipOffset(&dx, &dy)) return false;
    if (k != SLOT_Z && framesTipMPos(&tx, &ty)) m = k == SLOT_X ? tx : ty;
    if (k == SLOT_Z) m -= framesMeshOffset();
    *out = m + (k == SLOT_X ? dx : k == SLOT_Y ? dy : 0) - framesWork(k);
    return true;
}

float framesMeshOffset(void) {
    float x, y, dx, dy;
    if (!framesTipOffset(&dx, &dy) || !framesTipMPos(&x, &y)) return 0;
    return meshOffsetAt(x + dx, y + dy);
}

const char* framesToMachine(float wx, float wy, float* mx, float* my) {
    float dx, dy;
    if (!framesTipOffset(&dx, &dy)) return "no_head";
    *mx = wx + workX - dx;
    *my = wy + workY - dy;
    return nullptr;
}

const char* framesNamed(const char* name, float* mx, float* my) {
    const MachineCfg& c = machineCfg();
    if (strcmp(name, "park") == 0) {
        *mx = c.hasPark ? c.park.x : configParkPos(c.x);
        *my = c.hasPark ? c.park.y : configParkPos(c.y);
        return nullptr;
    }
    if (strcmp(name, "load") == 0) {
        if (!c.hasLoad) return "no_position";
        *mx = c.load.x;
        *my = c.load.y;
        return nullptr;
    }
    return "usage";
}

const char* framesProbeFor(uint8_t tool, float* mx, float* my) {
    const MachineCfg& c = machineCfg();
    const uint8_t sel = framesSelected();
    if (sel >= c.headCount) return "no_head";
    const CfgTool* t = configTool(c, tool);
    if (!t) return "no_tool";
    const CfgHead& h = c.heads[sel];
    if (!h.hasProbeSwitch) return "no_position";
    *mx = h.probeSwitch.x - h.xOffset - t->xOffset;
    *my = h.probeSwitch.y - h.yOffset - t->yOffset;
    return nullptr;
}

const char* framesCheckNode(uint8_t node, int32_t machineSteps) {
    if (!machineCfgValid()) return nullptr;
    const MachineCfg& c = machineCfg();
    const CfgAxis* found = nullptr;
    if (c.x.node.id == node) found = &c.x;
    if (c.y.node.id == node) found = &c.y;
    for (uint8_t h = 0; h < c.headCount; h++) {
        if (c.heads[h].z.node.id == node) found = &c.heads[h].z;
        if (c.heads[h].a.node.id == node) found = &c.heads[h].a;
    }
    float lo, hi;
    if (!found || !found->softLimits || !configAxisRange(*found, &lo, &hi)) return nullptr;
    const float u = (float)machineSteps / found->stepsPerUnit;
    const float m = found->invertDir ? -u : u;
    return (m < lo || m > hi) ? "soft_limit" : nullptr;
}

const char* framesCheckMove(uint8_t k, float from, float to, float* left) {
    static const char* const kWhy[3] = { "soft_limit x", "soft_limit y", "soft_limit z" };
    if (k > SLOT_Z) return nullptr;
    const CfgAxis* a = framesAxis(k);
    float lo, hi;
    if (!a || !a->softLimits || !(axes_homed & (1u << k)) || !framesTipRange(k, &lo, &hi))
        return nullptr;
    if (to >= lo && to <= hi) return nullptr;
    // Outside already (a blade mounted near the edge): it may hold or head in.
    if ((to < lo && to >= from) || (to > hi && to <= from)) return nullptr;
    *left = fmaxf(0.0f, to > from ? hi - from : from - lo);
    return kWhy[k];
}

const char* framesCheckXY(float mx, float my) {
    if (!machineCfgValid()) return nullptr;
    const MachineCfg& c = machineCfg();
    const CfgAxis* axes[2] = { &c.x, &c.y };
    const float v[2] = { mx, my };
    static const char* const kWhy[2] = { "soft_limit x", "soft_limit y" };
    for (uint8_t k = 0; k < 2; k++) {
        float lo, hi;
        if (!axes[k]->softLimits || !(axes_homed & (1u << k))) continue;
        if (framesTipRange(k, &lo, &hi) && (v[k] < lo || v[k] > hi)) return kWhy[k];
    }
    return nullptr;
}
