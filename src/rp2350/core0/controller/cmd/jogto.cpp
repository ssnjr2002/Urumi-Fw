// jogto.cpp — `jogto <x> <y>`, `jogto park|load|mount` and `probe_park <tool>`.
//
// The XY form takes work mm (framesToMachine), a name a stored position
// (framesNamed), probe_park the switch of the head the tool is mounted on (else
// the selected head's, selecting it first) under that tool's tip
// (framesProbeFor), with A to the holder's lock for a mount; each hands the
// anchor's machine XY to the recipe (controller/seq/jogto), which parks the Zs
// before the line. `ok` once it starts.

#include <Arduino.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include "table.h"
#include "../seq/jogto.h"
#include "../../config/machine_cfg.h"
#include "../../ops/axes_map.h"
#include "../../ops/frames.h"
#include "../../ops/tools.h"

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
    const char* why = jogtoStart(m[0], m[1], false);
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
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
    if (!why) why = jogtoStart(m[0], m[1], true);
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
    return true;
}
