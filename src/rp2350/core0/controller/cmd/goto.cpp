// goto.cpp — `goto <m|w> <xy|z|a> <pos…> [scale]`, `goto park|load|mount|probe
// [scale]` and `goto up|unlock`.
//
// m targets are machine units (X and Y the anchor's, as mpos), w targets work
// units (XY the selected tip, framesToMachine; Z and A framesWorkToMachine). A
// name is a stored position, or `probe` the selected head's switch under its
// mounted tool's tip (framesNamed). XY forms park the Zs first. Feeds are
// jogFeed × scale, capped at maxFeed. `ok` once it starts.

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

static const char* gotoXY(float mx, float my, float scale) {
    const MachineCfg& c = machineCfg();
    const float f = fminf(capped(c.x, scale), capped(c.y, scale));
    if (!(f > 0)) return "no_feed";
    return gotoLine(mx, my, f);
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
        return gotoXY(m[0], m[1], scale);
    }

    const bool work = w[0] == 'w';
    if (!word(&p, w, sizeof w)) return "usage";
    if (strcmp(w, "xy") == 0) {
        if (!parseArgs(p, v, 2, &scale)) return "usage";
        float m[2] = {v[0], v[1]};
        if (work) if (const char* why = framesToMachine(v[0], v[1], &m[0], &m[1])) return why;
        return gotoXY(m[0], m[1], scale);
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
