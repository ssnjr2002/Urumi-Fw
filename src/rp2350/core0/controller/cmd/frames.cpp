// frames.cpp — `select` and the work offset commands
// (docs/plans/coordinate-system.md). All from IDLE, PAUSED or ALARM.
//
//   select <head>|anchor     the controlled point; a head binds its Z and A
//   wzero [x] [y] [z]        work offset = the selected tip here (bare: all)
//   wset <axis> <v> …        work offset in machine units
//   wclear                   work offset back to the config's `work` block
//   mesh on|off              the bed mesh, from the next ring start

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "table.h"
#include "../../cmd/gate.h"
#include "../../config/machine_cfg.h"
#include "../../ops/axes_map.h"
#include "../../ops/frames.h"
#include "../../ops/mesh.h"
#include "../../ops/position.h"
#include "../../../ipc/shared_state.h"

static bool framesGateDenies() {
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

// Axis letter to slot (x, y, z), or SLOT_NONE.
static uint8_t axisSlot(const char* w, size_t len) {
    if (len != 1) return SLOT_NONE;
    switch (w[0]) {
        case 'x': return SLOT_X;
        case 'y': return SLOT_Y;
        case 'z': return SLOT_Z;
    }
    return SLOT_NONE;
}

// The next space-separated word of *p, advancing past it; its length in *len.
static const char* word(const char** p, size_t* len) {
    while (**p == ' ') (*p)++;
    const char* w = *p;
    while (**p && **p != ' ') (*p)++;
    *len = (size_t)(*p - w);
    return w;
}

bool cmdSelect(const char* args) {
    if (framesGateDenies()) return true;
    // A committed map ends a probe session; selecting is not the way out.
    if (machineState == STATE_PROBING) { Serial.println("err bad_state"); return true; }
    if (machineState == STATE_ALARM && alarmReason == ALARM_BUS_DEGRADED) {
        Serial.println("err degraded"); return true;
    }

    const MachineCfg& c = machineCfg();
    size_t len;
    const char* p = args;
    const char* w = word(&p, &len);
    size_t rest;
    word(&p, &rest);
    if (len == 0 || rest != 0) { Serial.println("err usage"); return true; }

    uint8_t head = FRAMES_NONE;
    if (len == 6 && strncmp(w, "anchor", 6) == 0) {
        if (c.laserNode) {
            // The laser has no slot: map X and Y if missing, Z and A as they are.
            const char* why = nullptr;
            if (axesReqAt(SLOT_X) == SLOT_NONE || axesReqAt(SLOT_Y) == SLOT_NONE) {
                uint8_t map[4];
                configSlotMap(c, c.defaultHead, SLOT_NONE, map);
                map[SLOT_Z] = axesReqAt(SLOT_Z);
                map[SLOT_A] = axesReqAt(SLOT_A);
                axesMapApply(map, /*keepWrongType=*/false, &why);
            }
            if (!framesSelectLaser() && !why) why = "unmapped";
            if (why) Serial.printf("err %s\n", why);
            else     Serial.println("ok");
            return true;
        }
        for (uint8_t h = 0; h < c.headCount && head == FRAMES_NONE; h++)
            if (c.heads[h].xOffset == 0.0f && c.heads[h].yOffset == 0.0f) head = h;
    } else {
        char* end;
        const unsigned long n = strtoul(w, &end, 10);
        if (end != w + len || n >= c.headCount) { Serial.println("err usage"); return true; }
        head = (uint8_t)n;
    }

    if (head == FRAMES_NONE) { Serial.println("err no_head"); return true; }   // decoder forbids
    uint8_t map[4];
    configSlotMap(c, head, SLOT_NONE, map);
    const char* why;
    axesMapApply(map, /*keepWrongType=*/false, &why);
    framesSelectHead(head);
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
    return true;
}

bool cmdWzero(const char* args) {
    if (framesGateDenies()) return true;
    bool want[3] = { false, false, false };
    if (*args == '\0') {
        want[SLOT_X] = want[SLOT_Y] = true;
        want[SLOT_Z] = framesZHead() != FRAMES_NONE;
    }
    for (const char* p = args; *p; ) {
        size_t len;
        const char* w = word(&p, &len);
        if (len == 0) break;
        const uint8_t k = axisSlot(w, len);
        if (k == SLOT_NONE) { Serial.println("err usage"); return true; }
        want[k] = true;
    }

    // Check every axis before setting any.
    float tip[3];
    float dx = 0, dy = 0;
    if ((want[SLOT_X] || want[SLOT_Y]) && !framesTipOffset(&dx, &dy)) {
        Serial.println("err no_head"); return true;
    }
    for (uint8_t k = 0; k < 3; k++) {
        if (!want[k]) continue;
        if (!(axes_homed & (1u << k))) { Serial.printf("err not_homed %c\n", "xyz"[k]); return true; }
        if (!framesMPos(k, &tip[k])) { Serial.println("err no_head"); return true; }
        tip[k] += k == SLOT_X ? dx : k == SLOT_Y ? dy : 0;
    }
    // XY first: the mesh's offset is relative to the new work origin. Z is
    // stored flat, so a Z zeroed away from the origin is not corrected twice.
    for (uint8_t k = 0; k < 2; k++)
        if (want[k]) framesSetWork(k, tip[k]);
    if (want[SLOT_Z]) framesSetWork(SLOT_Z, tip[SLOT_Z] - framesMeshOffset());
    Serial.println("ok");
    return true;
}

bool cmdWset(const char* args) {
    if (framesGateDenies()) return true;
    uint8_t ks[3];
    float vs[3];
    uint8_t n = 0;
    for (const char* p = args; *p; ) {
        size_t len;
        const char* w = word(&p, &len);
        if (len == 0) break;
        const uint8_t k = axisSlot(w, len);
        char* end;
        while (*p == ' ') p++;
        const float v = strtof(p, &end);
        if (k == SLOT_NONE || end == p || n == 3) { Serial.println("err usage"); return true; }
        p = end;
        ks[n] = k;
        vs[n++] = v;
    }
    if (n == 0) { Serial.println("err usage"); return true; }
    for (uint8_t i = 0; i < n; i++)
        if (ks[i] == SLOT_Z && framesZHead() == FRAMES_NONE) { Serial.println("err no_head"); return true; }
    for (uint8_t i = 0; i < n; i++) framesSetWork(ks[i], vs[i]);
    Serial.println("ok");
    return true;
}

bool cmdWclear(const char* args) {
    if (framesGateDenies()) return true;
    if (*args != '\0') { Serial.println("err usage"); return true; }
    framesClearWork();
    Serial.println("ok");
    return true;
}

bool cmdMesh(const char* args) {
    if (framesGateDenies()) return true;
    const bool on = strcmp(args, "on") == 0;
    if (!on && strcmp(args, "off") != 0) { Serial.println("err usage"); return true; }
    meshEnable(on);
    Serial.println("ok");
    return true;
}
