// mount.cpp — `mount <head> <tool id>` and `unmount <head>`: what a head has
// mounted, and its holder (docs/plans/position-moves.md, Holder lock). On a
// head with `unlockRange`, `mount` parks Z and turns A out of the range,
// locking the holder, and `unmount` parks Z and turns A to machine 0, unlocking it; both
// run as goto sequences. Without one, `mount` only records the tool. In a
// job's TOOL_WAIT any head, so the next layer's tool can go on with this one's.

#include <Arduino.h>
#include <math.h>
#include <stdlib.h>
#include "table.h"
#include "../seq/goto.h"
#include "../seq/steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/tools.h"

// Margin past unlockRange that `mount` turns A to, degrees.
#define LOCK_MARGIN_DEG 1.0f

// Up to `n` integers 0..255 and nothing after; false on anything else.
static bool parseBytes(const char* p, long* v, int n) {
    for (int i = 0; i < n; i++) {
        char* end;
        v[i] = strtol(p, &end, 10);
        if (end == p || v[i] < 0 || v[i] > 255) return false;
        p = end;
    }
    while (*p == ' ') p++;
    return *p == '\0';
}

static bool reply(const char* why) {
    if (why)
        Serial.printf("err %s\n", why);
    else
        Serial.println("ok");
    return true;
}

bool cmdMount(const char* args) {
    long v[2];
    if (!parseBytes(args, v, 2) || v[1] == 0) { Serial.println("err usage"); return true; }
    const uint8_t head = (uint8_t)v[0], id = (uint8_t)v[1];
    const MachineCfg& c = machineCfg();
    if (head >= c.headCount) return reply("no_head");
    if (!configTool(c, id)) return reply("no_tool");
    if (toolsMounted(head)) return reply("mounted");   // the holder takes one: unmount first

    const float range = c.heads[head].unlockRange;
    float at;
    if (range > 0 && !(aDeg(head, &at) && fabsf(at) > range))
        if (const char* why = gotoHead(head, true, true, range + LOCK_MARGIN_DEG))
            return reply(why);
    return reply(toolsMount(head, id));
}

bool cmdUnmount(const char* args) {
    long v[1];
    if (!parseBytes(args, v, 1)) { Serial.println("err usage"); return true; }
    const uint8_t head = (uint8_t)v[0];
    const MachineCfg& c = machineCfg();
    if (head >= c.headCount) return reply("no_head");
    if (const char* why = gotoHead(head, true, c.heads[head].unlockRange > 0, 0))
        return reply(why);
    return reply(toolsMount(head, 0));
}
