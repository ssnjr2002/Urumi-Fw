// jogto.cpp — `jogto <x> <y>` and `jogto park|load|probe`.
//
// The XY form takes work mm (framesToMachine), a name a stored position
// (framesNamed); both hand the anchor's machine XY to the recipe
// (controller/seq/jogto), which parks the Zs before the line. `ok` once it
// starts.

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include "table.h"
#include "../seq/jogto.h"
#include "../../ops/frames.h"

bool cmdJogTo(const char* args) {
    float m[2];
    if (isalpha((unsigned char)*args)) {
        if (const char* why = framesNamed(args, &m[0], &m[1])) {
            Serial.printf("err %s\n", why); return true;
        }
    } else {
        float v[2];
        char* end;
        const char* p = args;
        for (int i = 0; i < 2; i++) {
            v[i] = strtof(p, &end);
            if (end == p || !isfinite(v[i])) { Serial.println("err usage"); return true; }
            p = end;
        }
        while (*p == ' ') p++;
        if (*p) { Serial.println("err usage"); return true; }
        if (const char* why = framesToMachine(v[0], v[1], &m[0], &m[1])) {
            Serial.printf("err %s\n", why); return true;
        }
    }
    const char* why = jogtoStart(m[0], m[1]);
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
    return true;
}
