// Runtime config commands: `set config.<path> <value>` and `save`. `get
// config.<path>` is in cmd/get.cpp. See docs/plans/runtime-config.md.

#include <Arduino.h>
#include <string.h>
#include "table.h"
#include "../../config/machine_cfg.h"
#include "../../config/runtime_fields.h"
#include "../../ops/frames.h"
#include "../../ops/machine.h"
#include "../../ops/mesh.h"
#include "../../ops/position.h"
#include "../../../ipc/shared_state.h"

// K_WRITE has refused anything moving; a paused job and a swept estop are left.
static bool editRefused() {
    if (machineContext == CTX_IDLE && !estopLatched()) return false;
    Serial.println("err bad_state");
    return true;
}

static void runEffects(const RtApplied& a) {
    if (a.effects & RT_FX_voidDatum) {
        originInvalidate(a.node);
        if (a.axis == RT_AXIS_X || a.axis == RT_AXIS_Y) framesResetWorkXY();
        if (a.axis == RT_AXIS_Z) framesUnsetWorkZ(a.head);
    }
    if (a.effects & RT_FX_republishBuffer) machineCfgPublish();
    if (a.effects & RT_FX_applyMesh) meshEnable(machineCfg().meshOn);
}

bool cmdSet(const char* args) {
    if (strncmp(args, "config.", 7) != 0) { Serial.println("err usage"); return true; }
    const char* path = args + 7;
    const size_t len = strcspn(path, " ");
    const char* text = path + len;
    while (*text == ' ') text++;
    if (len == 0 || *text == '\0') { Serial.println("err usage"); return true; }
    if (editRefused()) return true;

    RtApplied a;
    const RtError e = machineCfgSet(path, len, text, &a);
    if (e == RT_RULE) {
        Serial.printf("err rule %s\n", configDecodeErrorName(a.rule));
    } else if (e != RT_OK) {
        Serial.printf("err %s\n", runtimeErrorName(e));
    } else {
        runEffects(a);
        Serial.println("ok");
    }
    return true;
}

bool cmdSave(const char* args) {
    if (*args != '\0') { Serial.println("err usage"); return true; }
    if (editRefused()) return true;
    const char* why = machineCfgSave();
    if (why) Serial.printf("err %s\n", why);
    else     Serial.println("ok");
    return true;
}
