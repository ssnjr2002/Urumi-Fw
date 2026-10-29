// goto.cpp — `goto <m|w> <xy|z|a> <pos…> [scale]`, `goto park|load|mount
// [scale]`, `goto up|unlock` and `probe_park <tool>`.
//
// m targets are machine units (X and Y the anchor's, as mpos), w targets work
// units (XY the selected tip, framesToMachine; Z and A framesWorkToMachine). A
// name is a stored position (framesNamed), probe_park the switch of the head
// the tool is mounted on (else the selected head's, selecting it first) under
// that tool's tip (framesProbeFor). XY forms park the Zs first; `mount` and
// probe_park turn the selected A to unlock (machine 0) along the line. Feeds
// are jogFeed × scale, capped at maxFeed. `ok` once it starts.

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "table.h"
#include "../seq/goto.h"
#include "../../config/machine_cfg.h"
#include "../../ops/axes_map.h"
#include "../../ops/frames.h"
#include "../../ops/position.h"
#include "../../ops/tools.h"

static float capped(const CfgAxis& a, float scale) {
    const float f = a.jogFeed * scale;
    return a.maxFeed > 0 && f > a.maxFeed ? a.maxFeed : f;
}

// `n` numbers then an optional positive scale, nothing after. False on any
// other shape.
static bool parseArgs(const char* p, float* v, int n, float* scale) {
    char* end;
    for (int i = 0; i < n; i++) {
        v[i] = strtof(p, &end);
        if (end == p || !isfinite(v[i])) return false;
        p = end;
    }
    *scale = strtof(p, &end);
    if (end == p) *scale = 1;
    else if (!(*scale > 0) || !isfinite(*scale)) return false;
    p = end;
    while (*p == ' ') p++;
    return *p == 0;
}

// The next word of `*p` into `w`, advancing past it. False when it is empty
// or too long.
static bool word(const char** p, char* w, size_t n) {
    while (**p == ' ') (*p)++;
    size_t i = 0;
    while (isalpha((unsigned char)**p)) {
        if (i + 1 >= n) return false;
        w[i++] = *(*p)++;
    }
    w[i] = 0;
    return i > 0;
}

static const char* gotoXY(float mx, float my, float scale, bool unlock) {
    const MachineCfg& c = machineCfg();
    const float f = fminf(capped(c.x, scale), capped(c.y, scale));
    if (!(f > 0)) return "no_feed";
    return gotoLine(mx, my, f, unlock);
}

static const char* run(const char* args) {
    if (!machineCfgValid()) return "unconfigured";
    const char* p = args;
    char w[8];
    if (!word(&p, w, sizeof w)) return "usage";
    float v[2], scale;

    if (strcmp(w, "up") == 0 || strcmp(w, "unlock") == 0) {
        while (*p == ' ') p++;
        if (*p) return "usage";
        return w[0] == 'u' && w[1] == 'p' ? gotoUp() : gotoUnlock();
    }
    if (strcmp(w, "m") != 0 && strcmp(w, "w") != 0) {
        if (!parseArgs(p, v, 0, &scale)) return "usage";
        float m[2];
        if (const char* why = framesNamed(w, &m[0], &m[1])) return why;
        return gotoXY(m[0], m[1], scale, strcmp(w, "mount") == 0);
    }

    const bool work = w[0] == 'w';
    if (!word(&p, w, sizeof w)) return "usage";
    if (strcmp(w, "xy") == 0) {
        if (!parseArgs(p, v, 2, &scale)) return "usage";
        float m[2] = {v[0], v[1]};
        if (work) if (const char* why = framesToMachine(v[0], v[1], &m[0], &m[1])) return why;
        return gotoXY(m[0], m[1], scale, false);
    }
    const uint8_t k = strcmp(w, "z") == 0 ? SLOT_Z : strcmp(w, "a") == 0 ? SLOT_A : 0xFF;
    if (k == 0xFF || !parseArgs(p, v, 1, &scale)) return "usage";
    const CfgAxis* ax = framesAxis(k);
    if (!ax) return "no_head";
    float to = v[0];
    if (work) {
        float from = 0;
        framesMPos(k, &from);
        if (const char* why = framesWorkToMachine(k, v[0], from, &to)) return why;
    }
    const float f = capped(*ax, scale);
    if (!(f > 0)) return "no_feed";
    return gotoAxis(k, to, f);
}

bool cmdGoto(const char* args) {
    if (const char* why = run(args)) Serial.printf("err %s\n", why);
    else                             Serial.println("ok");
    return true;
}

bool cmdProbePark(const char* args) {
    char* end;
    const unsigned long id = strtoul(args, &end, 10);
    while (*end == ' ') end++;
    if (end == args || *end || id == 0 || id > 0xFF) { Serial.println("err usage"); return true; }
    // The selected head if it has the tool, else the first that has it.
    const char* why = nullptr;
    const MachineCfg& c = machineCfg();
    const uint8_t sel = framesSelected();
    uint8_t head = sel;
    if (sel >= c.headCount || toolsMounted(sel) != id)
        for (uint8_t h = 0; h < c.headCount; h++)
            if (toolsMounted(h) == id) { head = h; break; }
    if (head != sel && head < c.headCount) {
        if (!c.heads[head].hasProbeSwitch) why = "no_position";
        else {
            axesSelectHead(head, &why);
            if (!why && framesSelected() != head) why = "no_head";
        }
    }
    float m[2];
    if (!why) why = framesProbeFor((uint8_t)id, &m[0], &m[1]);
    if (!why) why = gotoXY(m[0], m[1], 1, true);
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
    return true;
}
