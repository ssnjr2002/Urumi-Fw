// frames.cpp — see frames.h.

#include <Arduino.h>
#include <math.h>
#include "frames.h"
#include "position.h"
#include "../config/machine_cfg.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"

static uint8_t selected = FRAMES_NONE;
static float   workX = 0, workY = 0, workZ[CFG_MAX_HEADS] = {0};

static void laserSet(bool on) {
    if (!machineCfgValid() || !machineCfg().laserNode) return;
    rpcNodeCmd(CMD_LASER, machineCfg().laserNode, on ? 1u : 0u);   // best effort
}

void framesClearWork(void) {
    if (!machineCfgValid()) return;
    const MachineCfg& c = machineCfg();
    workX = c.workX;
    workY = c.workY;
    for (uint8_t h = 0; h < c.headCount; h++) workZ[h] = c.workZ[h];
}

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
    const CfgHead& h = machineCfg().heads[sel];
    *dx = h.xOffset;
    *dy = h.yOffset;
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
    return true;
}

bool framesWPos(uint8_t k, float* out) {
    float m;
    if (!framesMPos(k, &m)) return false;
    if (k == SLOT_A) {
        const float f = fmodf(fmodf(m, 360.0f) + 360.0f, 360.0f);
        *out = f > 180.0f ? f - 360.0f : f;
        return true;
    }
    float dx = 0, dy = 0;
    if (k != SLOT_Z && !framesTipOffset(&dx, &dy)) return false;
    *out = m + (k == SLOT_X ? dx : k == SLOT_Y ? dy : 0) - framesWork(k);
    return true;
}

const char* framesToMachine(float wx, float wy, float* mx, float* my) {
    float dx, dy;
    if (!framesTipOffset(&dx, &dy)) return "no_head";
    *mx = wx + workX - dx;
    *my = wy + workY - dy;
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
    if (!a || !a->softLimits || !(axes_homed & (1u << k)) || !configAxisRange(*a, &lo, &hi))
        return nullptr;
    if (to >= lo && to <= hi) return nullptr;
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
        if (configAxisRange(*axes[k], &lo, &hi) && (v[k] < lo || v[k] > hi)) return kWhy[k];
    }
    return nullptr;
}
