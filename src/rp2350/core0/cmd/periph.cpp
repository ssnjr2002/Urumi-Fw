// periph.cpp — the five peripheral actuator commands.
//
// They were five near-identical 28-line handlers: same gate, the same six-line
// gate comment pasted verbatim four times, same parse, same relay, same printf.
// Only the argument encoding differed. What is left is one relay plus five thin
// rows: vac_servo carries an extra index, knife_blower takes a range instead of
// on/off, and the other three are the bare on/off form.
//
// This file needs the transport and the parser. It does NOT touch the position
// model -- a peripheral holds no slot and has no datum.

#include <Arduino.h>
#include "table.h"
#include "parse.h"
#include "gate.h"
#include "../../ipc/core1_rpc.h"

// The shared gate, and the reason is Core 1's, not Core 0's. Core 0 no longer
// blocks on a relay (ipc/core1_rpc.h), but the RS485 exchange still runs on the
// core that owns the step budget and takes up to RESPONSE_TIMEOUT_MS -- many step
// intervals. Core 1 services channel 1 only between segments, so issuing one
// while a job is streaming is a dwell at a segment boundary, which leaves a mark
// in the material. Mid-job peripheral changes belong at a PAUSED boundary, which
// is where the host orchestrator issues them.
//
// KNOWN GAP (plan §7.2): machineState leaves RUNNING whenever the ring drains, so
// an underfed job sits in IDLE between refills and this gate admits the command
// in exactly the window it exists to close. The right predicate is "is a job in
// flight", which nothing represents today. Recorded in plan §11, not fixed here.
static inline bool periphGateDenies() {
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

// Every one of these answers with the same line, so the reply lives here too.
static bool relay(uint8_t cmd, uint8_t node, uint8_t arg) {
    Serial.printf("node %d %s\n", node, rpcResultText(rpcNodeCmd(cmd, node, arg)));
    return true;
}

// The bare form: `<cmd> <node> <on|off>`. Serves vac_pump, knife_osc, laser.
static bool relayOnOff(const char* args, uint8_t cmd) {
    if (periphGateDenies()) return true;
    char* end;
    uint8_t node = parseNode(args, &end);
    while (*end == ' ') end++;
    if (!node || *end == '\0') { Serial.println("err usage"); return true; }
    bool on = parseState(end);
    if (alarmDeniesOn(on)) return true;
    return relay(cmd, node, on ? 1u : 0u);
}

// ── vac_servo <node> <idx> <on|off> — vacuum-node servo channel ──────────────
// idx 0 = all servos, 1..6 = one. Packed high nibble = idx, low bit = on/off;
// Core 1 expands on → SERVO_ON_ANGLE before the wire (rpc_server.cpp).
bool cmdVacServo(const char* args) {
    if (periphGateDenies()) return true;
    char* end;
    uint8_t node = parseNode(args, &end);
    uint8_t idx  = (uint8_t)strtoul(end, &end, 10);
    while (*end == ' ') end++;
    if (!node || idx > 6 || *end == '\0') { Serial.println("err usage"); return true; }
    bool on = parseState(end);
    if (alarmDeniesOn(on)) return true;
    return relay(CMD_SERVO_SET, node, (uint8_t)((idx << 4) | (on ? 1u : 0u)));
}

// ── knife_blower <node> <0..100> — oscillating-knife blower PWM duty ─────────
// A range, not a toggle, so it cannot share relayOnOff. An absent duty token
// parses as 0, which turns the blower off — deliberate, and unchanged.
bool cmdKnifeBlower(const char* args) {
    if (periphGateDenies()) return true;
    char* end;
    uint8_t node = parseNode(args, &end);
    long    duty = strtol(end, &end, 10);
    if (!node || duty < 0 || duty > 100) { Serial.println("err usage"); return true; }
    if (alarmDeniesOn(duty > 0)) return true;
    return relay(CMD_KNIFE_BLOWER, node, (uint8_t)duty);
}

// ── vac_pump <node> <on|off> — vacuum-node SSR pump (soft-started) ───────────
bool cmdVacPump(const char* args) { return relayOnOff(args, CMD_SSR_SET); }

// ── knife_osc <node> <on|off> — oscillating-knife oscillator toggle ──────────
bool cmdKnifeOsc(const char* args) { return relayOnOff(args, CMD_KNIFE_OSC); }

// ── laser <node> <on|off> — stepper-node laser gate ─────────────────────────
// Only a node built -DNODE_HAS_LASER handles it; others now answer
// `nak unsupported` rather than not answering at all — the clearest single case
// of what the opcode buys, since the wrong-firmware node is otherwise
// indistinguishable from an absent one.
bool cmdLaser(const char* args) { return relayOnOff(args, CMD_LASER); }
