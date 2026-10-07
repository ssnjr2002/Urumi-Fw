// home.cpp — `home [only] [<node> …]` and its selectors (docs/homing.md).
//
// Each resolves a node list, checks it before anything moves, and hands it to
// the recipe (controller/seq/home). `ok` once the run starts; its progress and
// failure are in get (homing=, homecycle=, homefail=, homenode=).

#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include "table.h"
#include "../seq/home.h"
#include "../../config/machine_cfg.h"
#include "../../ops/homing.h"
#include "../../ops/position.h"
#include "../../../ipc/shared_state.h"

// Admitted from IDLE, or from the one alarm a leg leaves (LIMIT_LATCHED).
static bool homeGateDenies() {
    if (homeRunning() || homingHeld()) { Serial.println("err busy"); return true; }
    const bool admitted = machineState == STATE_IDLE ||
        (machineState == STATE_ALARM && alarmReason == ALARM_LIMIT_LATCHED);
    if (!admitted) { Serial.println("err bad_state"); return true; }
    return false;
}

static void homeRun(const uint8_t* nodes, uint8_t n, bool only) {
    if (n == 0) { Serial.println("ok"); return; }
    const char* why = homeStart(nodes, n, only);
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
}

static bool homeable(const CfgAxis* a) { return a && a->homing.present; }

// The next space-separated word of *p, advancing past it; its length in *len.
static const char* word(const char** p, size_t* len) {
    while (**p == ' ') (*p)++;
    const char* w = *p;
    while (**p && **p != ' ') (*p)++;
    *len = (size_t)(*p - w);
    return w;
}

bool cmdHome(const char* args) {
    if (homeGateDenies()) return true;
    const char* p = args;
    size_t len;
    const char* w = word(&p, &len);
    const bool only = len == 4 && strncmp(w, "only", 4) == 0;
    if (!only) p = args;

    uint8_t nodes[LEG_MAX];
    uint8_t n = 0;
    for (;;) {
        w = word(&p, &len);
        if (!len) break;
        char* end;
        const unsigned long v = strtoul(w, &end, 10);
        if (end != w + len || v == 0 || v > BUS_ADDR_MAX || n == LEG_MAX) {
            Serial.println("err usage"); return true;
        }
        for (uint8_t i = 0; i < n; i++)
            if (nodes[i] == v) { Serial.println("err usage"); return true; }
        nodes[n++] = (uint8_t)v;
    }
    for (uint8_t i = 0; i < n; i++) {
        const CfgAxis* a = homeAxisFor(nodes[i]);
        if (!a) { Serial.printf("err node %d not_in_config\n", nodes[i]); return true; }
        if (!homeable(a)) { Serial.printf("err node %d not_homeable\n", nodes[i]); return true; }
    }
    // No nodes: every homeable node of the config.
    if (n == 0) {
        const CfgAxis* axes[HOME_AXES_MAX];
        const uint8_t k = homeAxes(axes);
        for (uint8_t i = 0; i < k && n < LEG_MAX; i++)
            if (homeable(axes[i])) nodes[n++] = axes[i]->node.id;
    }
    homeRun(nodes, n, only);
    return true;
}

// home_unhomed: `home` of every homeable node with no origin; none is `ok`.
bool cmdHomeUnhomed(const char* args) {
    if (homeGateDenies()) return true;
    if (*args) { Serial.println("err usage"); return true; }
    const CfgAxis* axes[HOME_AXES_MAX];
    const uint8_t k = homeAxes(axes);
    uint8_t nodes[LEG_MAX];
    uint8_t n = 0;
    for (uint8_t i = 0; i < k && n < LEG_MAX; i++)
        if (homeable(axes[i]) && !originValid(axes[i]->node.id))
            nodes[n++] = axes[i]->node.id;
    homeRun(nodes, n, false);
    return true;
}

// home_cycle <k>: `home` of cycle k's nodes; an empty cycle is `err usage`.
bool cmdHomeCycle(const char* args) {
    if (homeGateDenies()) return true;
    char* end;
    const unsigned long c = strtoul(args, &end, 10);
    if (end == args || *end || c == 0 || c > 255) { Serial.println("err usage"); return true; }
    const CfgAxis* axes[HOME_AXES_MAX];
    const uint8_t k = homeAxes(axes);
    uint8_t nodes[LEG_MAX];
    uint8_t n = 0;
    for (uint8_t i = 0; i < k && n < LEG_MAX; i++)
        if (homeable(axes[i]) && axes[i]->homing.cycle == c)
            nodes[n++] = axes[i]->node.id;
    if (n == 0) { Serial.println("err usage"); return true; }
    homeRun(nodes, n, false);
    return true;
}

// home_head <n>: `home only <Zn> <An>`, n the index into heads[].
bool cmdHomeHead(const char* args) {
    if (homeGateDenies()) return true;
    char* end;
    const unsigned long h = strtoul(args, &end, 10);
    const MachineCfg& cfg = machineCfg();
    if (end == args || *end || h >= cfg.headCount) { Serial.println("err usage"); return true; }
    uint8_t nodes[2];
    uint8_t n = 0;
    const CfgAxis* both[2] = { &cfg.heads[h].z, &cfg.heads[h].a };
    for (const CfgAxis* a : both)
        if (a->node.present && homeable(a)) nodes[n++] = a->node.id;
    if (n == 0) { Serial.println("err usage"); return true; }
    homeRun(nodes, n, true);
    return true;
}
