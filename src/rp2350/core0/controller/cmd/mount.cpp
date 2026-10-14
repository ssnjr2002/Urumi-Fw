// mount.cpp — `mount <head> <tool id>`: what a head has mounted (0 = none).
// Never moves; separate from `select`, so a head can be given a tool without
// being the controlled point.

#include <Arduino.h>
#include <stdlib.h>
#include "table.h"
#include "../../ops/tools.h"

bool cmdMount(const char* args) {
    long v[2];
    const char* p = args;
    for (int i = 0; i < 2; i++) {
        char* end;
        v[i] = strtol(p, &end, 10);
        if (end == p || v[i] < 0 || v[i] > 255) { Serial.println("err usage"); return true; }
        p = end;
    }
    while (*p == ' ') p++;
    if (*p) { Serial.println("err usage"); return true; }
    if (const char* why = toolsMount((uint8_t)v[0], (uint8_t)v[1]))
        Serial.printf("err %s\n", why);
    else
        Serial.println("ok");
    return true;
}
