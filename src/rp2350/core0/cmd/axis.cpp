// axis.cpp — the commands that bind, datum, energise, or step an axis.
//
// This is the ONLY file that writes the position model. Before the split,
// origin invalidation was scattered across disable, axes_enable, bus_enable,
// axis_map, setorigin and reconcileValidity inside one 1000-line file, and the
// recurring bug the source comments describe is exactly "updated one frame,
// forgot the other". Now there is one file to audit, and it reaches the model
// only through position.h's named operations.

#include <Arduino.h>
#include <string.h>            // memcpy
#include <stdlib.h>            // strtol / strtoul
#include "table.h"
#include "parse.h"
#include "gate.h"
#include "../position.h"
#include "../homing.h"
#include "../probe.h"
#include "axis_map.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"
#include "hardware/sync.h"     // __dmb

// The gate shared by every command here that goes to the bus: Core 1 services
// channel 1 only after draining the ring, so a request issued mid-stream waits
// out the whole queue while Core 0 blocks and stops reading serial.
static inline bool busGateDenies() {
    if (!stateIs(STATE_IDLE, STATE_PAUSED, STATE_ALARM)) {
        Serial.println("err bad_state");
        return true;
    }
    return false;
}

// ── axes_enable <on|off> (IDLE/PAUSED/ALARM) ─────────────────────────────────
// Targets the axis map: every node currently bound to a motion slot, and no one
// else. This replaces the old `enable all` / `disable all`, whose name read
// bus-wide while the code always walked the axis map — a distinction that
// stopped being academic once vacuum and knife nodes joined the bus.
// Peripherals hold no slot, so they are addressed only by `enable <id>`.
bool cmdAxesEnable(const char* args) {
    if (busGateDenies()) return true;
    if (*args == '\0') { Serial.println("err usage"); return true; }
    // ALARM_CONFIG refuses: this command's target IS the axis map, and under the
    // config gate there is no committed map to operate on. Before the gate became
    // bidirectional (cmdAxisMap below) it walked zero slots and answered `ok`,
    // which reads as "the axes are now off" on a machine that has no axes.
    // Same string as `unalarm`, which refuses the same state for the same reason.
    if (machineState == STATE_ALARM && alarmReason == ALARM_CONFIG) {
        Serial.println("err unconfigured"); return true;
    }
    // Still NOT gated by alarmDeniesOn, and not by ALARM generally: ALARM is
    // where axis recovery happens, and the post-estop flow is axes_enable on →
    // setorigin → unalarm. That path runs under ALARM_ESTOP, so the refusal
    // above does not touch it. `enable <id>` stays ungated in every alarm —
    // it addresses a bus node directly rather than through the map, which is how
    // peripherals are reached and does not depend on a map existing at all. The
    // peripheral commands gate because energising a pump under alarm has no
    // recovery role.
    bool on = parseState(args);       // accepts "1"/"on" and "0"/"off"
    for (uint8_t i = 0; i < MOTION_SLOTS; i++) {
        uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE) continue;
        // No bookkeeping here: Core 1 folds each ack into nodeEnabled as it
        // relays, and reconcileValidity projects that onto the slots before the
        // next command is served (ipc/shared_state.h).
        rpcNodeCmd(on ? CMD_ENABLE : CMD_DISABLE, n, 0);
    }
    // De-energised → back-drivable → every bound origin is void. Keyed on the
    // node, not the slot, so a node that loses holding torque while PARKED
    // still loses its origin (see originInvalidate).
    if (!on)
        for (uint8_t i = 0; i < MOTION_SLOTS; i++)
            if (slotNodeAt(i) != SLOT_NONE) originInvalidate(slotNodeAt(i));
    Serial.println("ok");
    return true;
}

// ── bus_enable <on|off> (IDLE/PAUSED/ALARM) ──────────────────────────────────
// Whole-bus broadcast: ONE unacknowledged frame reaches every node at once,
// peripherals included. This is the genuinely bus-wide verb that the old
// `enable all` only claimed to be; `axes_enable` remains the axis-map form.
//
// Nobody answers a broadcast, so this cannot learn what actually happened. The
// energisation half of that asymmetry now lives where the frame goes out
// (core1/rpc_server.cpp's broadcast path): off clears nodeEnabled, on touches
// nothing. What stays here is the half Core 1 has no business in — origins are
// Core 0's, and a de-energised bus is back-drivable, so every datum dies.
bool cmdBusEnable(const char* args) {
    if (busGateDenies()) return true;
    if (*args == '\0') { Serial.println("err usage"); return true; }
    bool on = parseState(args);
    rpcNodeCmd(on ? CMD_ENABLE : CMD_DISABLE, BUS_ADDR_BROADCAST, 0);
    if (!on) originInvalidateAll();
    Serial.println("ok");
    return true;
}

// ── enable / disable <id> (IDLE/PAUSED/ALARM) ────────────────────────────────
// Relays to any bus node — the generic CMD_ENABLE effect is delegated per type
// (motor energize / pump on …); the axis bookkeeping applies only when the id
// is an axis node (docs/engage_and_axis_map.md §9).
bool cmdEnable(const char* args) {
    if (busGateDenies()) return true;
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err bad_node"); return true; }
    // The axis bookkeeping this used to do by hand is gone: it keyed on the slot
    // and set the bit unconditionally, discarding the RpcResult entirely — so a
    // node that timed out still printed `ok` and still read as energised. Core 1
    // now records it from the ack, and only from an ack.
    rpcNodeCmd(CMD_ENABLE, node, 0);
    Serial.println("ok");
    return true;
}

bool cmdDisable(const char* args) {
    if (busGateDenies()) return true;
    uint8_t node = parseNode(args, nullptr);
    if (!node) { Serial.println("err bad_node"); return true; }
    rpcNodeCmd(CMD_DISABLE, node, 0);
    // Unconditional: a de-energised node is back-drivable whether or not
    // it currently holds a slot, so its origin is void either way. This
    // is exactly the case slot-indexed bookkeeping could not express —
    // a PARKED head losing holding torque and sagging under gravity.
    // (The energisation bit itself is Core 1's now; only the origin is ours.)
    originInvalidate(node);
    Serial.println("ok");
    return true;
}

// ── axis_map [<x> <y> <z> <a>] — bind bus nodes to stream slots ──────────────
// No-arg: read back the committed map in setter syntax ('-' = unbound slot).
// Four tokens (a bus id, or '-'/'0' = unbound). Committing a map with at least
// one slot bound clears the ALARM_CONFIG boot gate; committing an empty one
// re-enters it. Valid IDLE/PAUSED/ALARM; rebinding mid-RUNNING corrupts motion
// (§6.2).
bool cmdAxisMap(const char* args) {
    if (*args == '\0') {                          // read-back form
        Serial.print("axis_map");
        for (int i = 0; i < 4; i++) {
            if (slotNodeAt(i) == SLOT_NONE) Serial.print(" -");
            else                            Serial.printf(" %d", slotNodeAt(i));
        }
        Serial.println();
        return true;
    }

    if (busGateDenies()) return true;

    // Parse exactly four tokens into desired[]: a bus id, or '-'/'0' = unbound.
    uint8_t desired[4];
    const char* p = args;
    for (int i = 0; i < 4; i++) {
        while (*p == ' ') p++;
        if (*p == '\0') { Serial.println("err usage"); return true; }
        if (*p == '-') { desired[i] = SLOT_NONE; p++; continue; }
        char* endPtr;
        unsigned long v = strtoul(p, &endPtr, 10);
        if (endPtr == p) { Serial.println("err usage"); return true; }
        p = endPtr;
        if (v == 0)                 desired[i] = SLOT_NONE;
        else if (v <= BUS_ADDR_MAX) desired[i] = (uint8_t)v;
        else { Serial.println("err bad_node"); return true; }
    }
    // A bus id can occupy only one slot — reject a node bound twice.
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            if (desired[i] != SLOT_NONE && desired[i] == desired[j]) {
                Serial.println("err dup"); return true;
            }

    // Committing a map is also the OTHER way out of a probe session (§5.5):
    // during a probe the machine genuinely has no working axis map, and the way
    // out of that condition has always been to commit one. This is not an
    // overload of `axis_map` -- it is the ALARM_CONFIG parallel taken seriously.
    // Any committed map ends the session, and `probe_end` is sugar for
    // committing the one that was already there.
    if (machineState == STATE_PROBING) return probeExit(desired);

    axisMapApply(desired, /*quiet=*/false);
    return true;
}

bool axisMapApply(const uint8_t* desired, bool quiet) {
    // NOT a diff — deliberately dumb. First disengage every previously-bound
    // node (best-effort: a since-removed/reset node that won't ACK is already
    // where we want it), then engage EVERY desired node to its slot,
    // unconditionally. Re-issuing the same axis_map therefore re-sends every
    // engage, so a node that silently lost its slot (reflash / power blip /
    // fresh Pico map) is always re-bound — the node state can never drift from
    // what the map claims, which a skip-if-unchanged diff allowed.
    // Park every bound node, recording the counter each reports (position.h,
    // the frozen-while-parked check).
    for (int i = 0; i < 4; i++) {
        uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE) continue;
        NodeStatus st;
        if (rpcNodeStatus(CMD_ENGAGE, n, SLOT_NONE, &st) == RPC_OK &&
            st.hasStepperTail)
            parkRecord(n, st.pos);
        else
            // No answer — we do not know where it stopped. Drop any earlier
            // entry rather than let a stale one produce a false match later.
            parkForget(n);
    }

    // Engage, and adopt each slot's state straight out of the ack — position
    // and enabled bit in the same transaction as the bind. This is the whole
    // point of the node-frame datum: a head parked through several rebinds
    // comes back with its position intact, and a slot that changed hands never
    // inherits the previous occupant's count.
    for (int i = 0; i < 4; i++) {
        if (desired[i] == SLOT_NONE) continue;
        NodeStatus st;
        RpcResult r = rpcNodeStatus(CMD_ENGAGE, desired[i], (uint8_t)i, &st);
        if (r != RPC_OK) {
            // `nak unsupported` here means a non-stepper node was mapped to a
            // motion slot — a config error, not a bus fault.
            if (!quiet) Serial.printf("err node %d %s\n", desired[i], rpcResultText(r));
            return false;             // leave the map as far as it got
        }
        // Frozen-while-parked check (position.h). Silent by design: the
        // wire contract is exactly one line per command, so this cannot print.
        // Clearing the node's origin is the report — the axis comes back
        // un-homed, which getpos's mask and getstate both surface.
        if (st.hasStepperTail && parkMoved(desired[i], st.pos))
            originInvalidate(desired[i]);       // moved while parked
        slotBind((uint8_t)i, desired[i], &st);
    }
    // Slots left unbound hold no node, so they hold no position either.
    for (int i = 0; i < 4; i++) {
        if (desired[i] != SLOT_NONE) continue;
        slotUnbind((uint8_t)i);
    }

    // Committed. The config gate tracks the map both ways.
    //
    // A map with nothing bound is not a configured machine, and `axis_map - - - -`
    // is a legitimate way to reach one -- it parses, it commits, and every slot
    // ends unbound. Clearing ALARM_CONFIG on that would leave the machine IDLE
    // with no axis bound, and motion ingest gates on machineState alone
    // (data_plane.cpp), so it would then accept a job and emit stream bytes that
    // no node is listening to, advancing machinePos for axes that do not exist.
    //
    // So the gate is re-entered, not merely left un-cleared: the map can go from
    // configured to unconfigured, and the state has to be able to follow it back.
    // The command still answers `ok` -- committing an empty map is what was asked
    // for, and it succeeded. That the result is an unconfigured machine is a state
    // fact, and state facts travel as reason codes here, not as command errors.
    bool anyBound = false;
    for (uint8_t i = 0; i < MOTION_SLOTS; i++)
        if (slotNodeAt(i) != SLOT_NONE) { anyBound = true; break; }

    if (!anyBound) {
        // Reason before state, matching how Core 1 publishes the pair.
        alarmReason  = ALARM_CONFIG;
        __dmb();
        machineState = STATE_ALARM;
    } else if (machineState == STATE_ALARM && alarmReason == ALARM_CONFIG) {
        machineState = STATE_IDLE;
        alarmReason  = ALARM_NONE;
    }
    if (!quiet) Serial.println("ok");
    return true;
}


// ── setorigin [axes] [pos_steps] (IDLE/PAUSED/ALARM) ─────────────────────────
//
// `pos_steps` is the machine position the axes are AT right now, defaulting to
// 0. Zero is the switch-at-origin case; a far-end switch needs
// hardTravel × stepsPerUnit, which the old zero-only form could not express
// (docs/homing.md §2.5). It is the WIRE frame — steps, signed — because
// machinePos is; the host owns the mm conversion.
//
// The datum stays here rather than folding into `home` so that `home` remains
// purely about motion: the retract and slow re-approach passes carry no datum
// baggage, and this inherits the estop-window handling below, which is subtle
// enough that it should not exist twice.
bool cmdSetOrigin(const char* args) {
    if (busGateDenies()) return true;

    // Split at the first space: axisMask() scans every character it is given,
    // so handing it the whole line would let a stray letter in a later argument
    // select an axis nobody named.
    const char* p = args;
    while (*p && *p != ' ') p++;
    char axesTok[8];
    size_t n = (size_t)(p - args);
    if (n >= sizeof(axesTok)) { Serial.println("err usage"); return true; }
    memcpy(axesTok, args, n);
    axesTok[n] = '\0';

    int32_t posSteps = 0;
    while (*p == ' ') p++;
    if (*p) {
        char* end;
        long v = strtol(p, &end, 10);
        if (end == p) { Serial.println("err usage"); return true; }
        posSteps = (int32_t)v;
    }

    uint8_t m = axisMask(axesTok);
    // setorigin does bus I/O below — up to four round trips, so it can be in
    // flight for tens of milliseconds. An estop landing inside that window
    // would otherwise be ERASED by the alarm-clearing block at the end, which
    // cannot tell "the fault I was invoked to recover from" apart from "a
    // fault that arrived while I was working". Snapshot the reason on entry
    // and only clear what we came in with.
    uint8_t alarmAtEntry = alarmReason;
    // The datum is recorded in the NODE's frame: originRecord captures that
    // node's own counter here, so machinePos is a derived offset from now on
    // and survives any later rebinding. A masked slot with no node bound
    // cannot be datumed — there is nothing to record against — so it is
    // skipped and left un-homed rather than silently claiming an origin.
    // Named axes that resolved to a node. A mask where NOTHING resolved did no
    // work at all, and answering `ok` to that reports a datum that was never
    // recorded -- the operator reads back the old position and has to guess why.
    // Counted rather than pre-checked so the per-slot skip above stays intact
    // for a partly-bound `setorigin` with no axis token, which is the common
    // case and is not an error.
    uint8_t bound = 0;
    for (int i = 0; i < 4; i++) {
        if (!(m & (1 << i))) continue;
        uint8_t n = slotNodeAt(i);
        if (n == SLOT_NONE) { axes_homed &= ~(1 << i); continue; }
        bound++;

        // CMD_DATUM_SET arms the node's continuity witness AND returns the
        // counter it refers to. One transaction, so the origin recorded here
        // and the witness armed there describe the same instant — a separate
        // read could straddle a reset and pair a witness with a stale count.
        NodeStatus st;
        if (rpcNodeStatus(CMD_DATUM_SET, n, 0, &st) != RPC_OK ||
            !st.hasStepperTail || !(st.flags & NODE_FLAG_DATUM)) {
            originInvalidate(n);           // no answer, or witness not armed
            continue;
        }
        originRecord(n, st.pos, posSteps);
    }
    if (bound == 0) { Serial.println("err unbound"); return true; }
    // A fault that arrived while we were on the bus outranks this command. The
    // datum we just recorded describes a machine that has since stopped hard,
    // so refuse rather than clear it — reconcileValidity() drops the masks on
    // the next pass, and the operator retries after unalarm.
    if (alarmReason != alarmAtEntry || machineState == STATE_ESTOP) {
        Serial.println("err estop"); return true;
    }
    // setorigin recovers from an ESTOP-alarm, but NOT the config gate — only a
    // committed axis_map clears ALARM_CONFIG (docs/engage_and_axis_map.md §6.1).
    // ...and not out of a latched limit either, for the same reason unalarm
    // cannot: recording a datum does not move the axis off the switch.
    if (machineState == STATE_ALARM && alarmReason != ALARM_CONFIG) {
        resumeOrHold();
    }
    Serial.println("ok");
    return true;
}

// ── lin_leg / rot_leg <node> <dir> <start_us> <floor_us> <ramp> <max> [intent]
//
// docs/homing.md §2.2. ONE LEG, NOT A HOME. The firmware runs a leg and reports
// what it measured; sequencing legs into a home, deciding when a pair is done,
// and turning the result into a datum all belong to the host (§3). The verb says
// so, which the old `home` did not: for a linear axis `home` was one leg of
// four, and for a rotary one it looked like the whole job. Two verbs also mean
// no argument means two things -- `rot_leg` has no `intent` because there is no
// pin to predict.
//
// ADDRESSES A BUS ID, not an axis. Every output of a leg is node-framed: the
// span, the index in the node's own counter, the limit latch (a switch is wired
// to a NODE). Not one of them is slot-framed. position.h's rule is that the node
// frame is the truth and the slot frame is a view, so routing a command that
// writes only truths through a view was backwards -- and it cost a real thing on
// the bench: homing a node that no slot claimed needed a throwaway
// `axis_map - - - 4` to borrow a slot first. It also means the config gate is
// gone from here. An axis cannot be resolved without a committed map, but a node
// id needs no map at all, so a leg now runs during commissioning -- which is
// exactly when homing matters.
//
// The datum survives either way: originInvalidate() is node-framed and clears
// the slot's homed bit only if a slot happens to point here (position.cpp), and
// slotAdoptStatus recomputes machinePos from nodeOrigin on every later bind. A
// leg run before the map and a map committed after it land correctly.
//
// Still raw and positional in its numbers: no mm, no steps/mm, no config lookup,
// no `invert`. Composing those belongs to the host (§3), and a temporary
// Pico-side version of them is exactly how they end up living here permanently.
// The Pico relays and supervises; it does not plan.
//
// NO <seek|retract> ARGUMENT THAT STEERS ANYTHING on the linear side. The node
// picks the mode from one read of its own limit pin at arm time (§1.2), which
// reproduces §3.4's seek → retract → seek sequence on its own: after a seek the
// switch is asserted, so the next leg retracts; after the back-off it is clear,
// so the next one seeks. WHICH mode ran -- needed to interpret the terminal
// flags -- comes back in the arm ack; see homingBegin().
//
// `intent` is the host's prediction of that same thing, checked rather than
// obeyed: the host's own plan (§3.4) knows whether this leg is SUPPOSED to start
// on the switch, so it says so, and the node NAKs (NAK_INTENT_MISMATCH) rather
// than silently running under the wrong leg's budget semantics. The Pico is a
// pure relay for it.

// Everything both verbs share: gates, the node token, and the six numbers.
// `intent` is parsed by the caller because only one verb has it.
static bool legCommon(const char* args, uint8_t expectKind, bool wantIntent) {
    // BEFORE the bus gate, which does not admit STATE_HOMING and would answer
    // the commonest mistake here -- a second leg while one is in flight -- with
    // a generic `bad_state`. Same refusal either way; this one names what to
    // wait for, and putting it second made it unreachable.
    if (homingActive()) { Serial.println("err busy"); return true; }
    if (busGateDenies()) return true;

    char* end;
    const unsigned long nodeV = strtoul(args, &end, 10);
    if (end == args || nodeV > BUS_ADDR_MAX) { Serial.println("err usage"); return true; }
    const uint8_t node = (uint8_t)nodeV;
    const char* p = end;

    // A de-energised node accepts CMD_HOME_LEG and pulses into a motor that
    // cannot turn: the terminator is never reached, so the leg burns its entire
    // max_steps budget -- tens of seconds on a seek -- and then reports a
    // failure that reads as a broken switch rather than a motor nobody turned
    // on.
    //
    // This is the gate ALARM does not provide and should not: a leg is admitted
    // in ALARM because homing is how an operator recovers from an estop, and the
    // estop sweep de-energises the bus on its way in. The two are separate facts
    // -- "the machine faulted" and "this axis can move" -- and only the second
    // decides whether a leg is worth arming.
    //
    // Reads nodeEnabled, not axes_enabled. axes_enabled is only the projection
    // of this mask through the axis map (position.cpp), so on a bound node the
    // two agree, and on an unbound one only this exists.
    if (!(nodeEnabled & (1u << node))) {
        Serial.println("err not_enabled"); return true;
    }

    const int want = wantIntent ? 6 : 5;
    unsigned long v[6] = {0};
    for (int i = 0; i < want; i++) {
        v[i] = strtoul(p, &end, 10);
        if (end == p) { Serial.println("err usage"); return true; }
        p = end;
    }
    if (v[0] > 1 || v[1] > 0xFFFF || v[2] > 0xFFFF || v[3] > 0xFFFF || v[5] > 1) {
        Serial.println("err range"); return true;
    }
    // A zero interval would divide the pulser's ramp by nothing and free-run the
    // step pin; a zero budget is a command that cannot move and cannot fail.
    if (v[1] == 0 || v[2] == 0 || v[4] == 0) { Serial.println("err range"); return true; }

    return homingBegin(node, expectKind, (uint8_t)(v[0] & 1), v[5] != 0,
                       (uint16_t)v[1], (uint16_t)v[2],
                       (uint16_t)v[3], (uint32_t)v[4]);
}

bool cmdLinLeg(const char* args) {
    return legCommon(args, HOMING_KIND_LIMIT, true);
}

// No `intent`: a rotary node has no limit pin, so there is nothing for the host
// to predict and nothing for the node to disagree with. It passes false, and
// the node's intent check is dead code on an index build.
bool cmdRotLeg(const char* args) {
    return legCommon(args, HOMING_KIND_INDEX, false);
}

// ── step <node> <count> [sps] — debug stepping (bring-up only) ───────────────
// <node> is a BUS id resolved to its ENGAGE-bound stream slot via the axis map,
// so the node must be in a committed axis_map first. count is a full int32, its
// sign the direction, clamped to STEP_DEBUG_MAX. [sps] defaults to
// STEP_DEBUG_SPS and is clamped to STEP_DEBUG_SPS_MAX.
//
// The datum SURVIVES a debug burst. The node is engaged, so it counts these
// bytes into its own position exactly as during a job, and Core 1 adds the
// same steps to machinePos — both frames stay consistent. This used to clear
// axes_homed, which made sense only while position was slot-framed.
//
// Hence the enabled requirement: a node counts stream bytes whether or not its
// motor is energised, so stepping a de-energised axis would advance both
// counters while the shaft stayed put — the one case where the two agree and
// are both wrong. Refuse it rather than record a fiction.
bool cmdStep(const char* args) {
    if (busGateDenies()) return true;
    char* end;
    uint8_t node = (uint8_t)strtoul(args, &end, 10);
    long count = strtol(end, &end, 10);
    if (count == 0) { Serial.println("err usage"); return true; }
    if (labs(count) > STEP_DEBUG_MAX) { Serial.println("err too_many"); return true; }
    uint32_t sps = strtoul(end, &end, 10);   // optional — 0 if absent
    if (sps == 0) sps = STEP_DEBUG_SPS;
    if (sps > STEP_DEBUG_SPS_MAX) sps = STEP_DEBUG_SPS_MAX;
    uint8_t slot = nodeSlot(node);
    if (slot == SLOT_NONE) { Serial.println("err not_engaged"); return true; }
    if (!(axes_enabled & (1 << slot))) {
        Serial.println("err not_enabled"); return true;
    }
    // Both parameters ride the request so back-to-back `step`s cannot steal
    // each other's rate — see ipc/core1_rpc.h.
    rpcStepDebug(slot, (uint16_t)sps, (int32_t)count);
    Serial.printf("ok %ld steps %lu sps\n", count, (unsigned long)sps);
    return true;
}

// ── hallscan <node> <stepPer> <samples> [sps] — field profile vs position ────
// The bring-up instrument the node port never had. src/scratch/hall_capture.py
// could plot the A1324's output against step count before choosing a single
// threshold; on the bus there was no equivalent, so HOME_ENTER, HOME_WIN and
// HOME_DECIM were carried over from node 4's captures and applied to node 5 on
// faith. Three bench runs failed in three different ways for the same reason:
// nobody had seen node 5's waveform. This shows it.
//
// It steps and reads rather than sampling during a move, so it is SLOW and the
// profile it draws is a static one -- which is the point. It answers, without
// any thresholding in the way: how deep is the dip, how wide is it, is there
// exactly one per revolution, and how many steps IS a revolution on this head.
//
// ONE line of output, not one per sample. The text plane is strictly
// request/response and the host reads exactly one line per command; a multi-line
// reply desyncs it for the rest of the session, which is the same trap
// documented at cmdPingNode. Paste the line into a plot.
#define HALLSCAN_MAX 300

bool cmdHallScan(const char* args) {
    if (busGateDenies()) return true;
    char* end;
    uint8_t node = (uint8_t)strtoul(args, &end, 10);
    long stepPer = strtol(end, &end, 10);
    long samples = strtol(end, &end, 10);
    if (!node || stepPer == 0 || samples <= 0) { Serial.println("err usage"); return true; }
    if (samples > HALLSCAN_MAX) { Serial.println("err too_many"); return true; }
    uint32_t sps = strtoul(end, &end, 10);
    if (sps == 0) sps = STEP_DEBUG_SPS;
    if (sps > STEP_DEBUG_SPS_MAX) sps = STEP_DEBUG_SPS_MAX;

    uint8_t slot = nodeSlot(node);
    if (slot == SLOT_NONE) { Serial.println("err not_engaged"); return true; }
    // Same reason as cmdStep: a de-energised node still counts stream bytes, so
    // an unenabled scan would advance the count while the shaft -- and therefore
    // the field -- stayed put, drawing a flat line that looks like a dead sensor.
    if (!(axes_enabled & (1 << slot))) { Serial.println("err not_enabled"); return true; }

    NodeStatus st;
    if (rpcNodeStatus(CMD_NODE_STATUS, node, 0, &st) != RPC_OK || !st.hasIndex) {
        Serial.println("err no_index");    // not a rotary build, or not answering
        return true;
    }

    Serial.printf("hallscan node %d step %ld n %ld from %ld vals",
                  node, stepPer, samples, (long)st.pos);
    for (long i = 0; i < samples; i++) {
        Serial.printf(" %d", st.hallRaw);
        rpcStepDebug(slot, (uint16_t)sps, (int32_t)stepPer);

        // Wait for the burst to land rather than trusting a computed duration.
        // rpcStepDebug is fire-and-forget (no reply, no in-flight slot), so the
        // only evidence the steps were emitted is the node's OWN count reaching
        // the target -- which is also the check that would catch the axis being
        // stalled or the stream not arriving at all.
        const int32_t want = st.pos + (int32_t)stepPer;
        const uint32_t deadline = millis() + (uint32_t)(labs(stepPer) * 1000 / sps) + 500;
        do {
            if (rpcNodeStatus(CMD_NODE_STATUS, node, 0, &st) != RPC_OK) {
                Serial.println(" err bus"); return true;
            }
        } while (st.pos != want && (int32_t)(millis() - deadline) < 0);
        if (st.pos != want) { Serial.println(" err stalled"); return true; }
    }
    Serial.printf(" %d end %ld\n", st.hallRaw, (long)st.pos);
    return true;
}

// ── probe_map <stepper-id> <switch-id> ───────────────────────────────────────
// Open a probe session. See core0/probe.h for why this is a session and not a
// single command, and docs/tool_probe.md §5.3 for why it is a full alternative
// binding rather than an overlay.
bool cmdProbeMap(const char* args) {
    if (probeActive()) { Serial.println("err busy"); return true; }
    if (busGateDenies()) return true;
    char* end;
    const uint8_t z   = parseNode(args, &end);
    const uint8_t vac = parseNode(end,  &end);
    if (!z || !vac) { Serial.println("err bad_node"); return true; }
    return probeBegin(z, vac);
}

// ── probe_leg <dir> <start_us> <ceil_us> <ramp_steps> <poll_div> <max_steps>
//             <deadline_us> <intent> ─────────────────────────────────────────
// Positional, following lin_leg's idiom. The host owns the sequence and the Pico
// runs ONE leg: there is no leg index, and the Pico never learns which leg of
// four this is.
//
// No node argument, and that is the difference from lin_leg rather than an
// oversight. A home is node-framed and runs before any map is committed; a probe
// is the opposite — the session already bound both nodes, so naming them again
// would be a second source of truth that could disagree with the binding.
//
// `start_us` is not in the design document's argument table. It is here for the
// reason lin_leg carries both a start and a floor: a ramp needs somewhere to
// ramp FROM, and the alternative was a hidden multiplier of ceil_us buried in
// the emitter, which is a worse place for a number that decides whether Z loses
// steps.
bool cmdProbeLeg(const char* args) {
    // The bus gate does not admit STATE_PROBING -- correctly, since it is a
    // motion state for every other command. Checked first so the commonest
    // mistake here, a leg with no session, is named rather than answered with a
    // generic bad_state.
    if (machineState != STATE_PROBING) { Serial.println("err not_probing"); return true; }
    char* end;
    const char* p = args;
    unsigned long v[8];
    for (int i = 0; i < 8; i++) {
        v[i] = strtoul(p, &end, 10);
        if (end == p) { Serial.println("err usage"); return true; }
        p = end;
    }
    if (v[0] > 1 || v[1] > 0xFFFF || v[2] > 0xFFFF || v[3] > 0xFFFF ||
        v[4] > 0xFF || v[6] > 0xFFFF || v[7] > 1) {
        Serial.println("err range"); return true;
    }
    // Zero is rejected for the interval, the poll divisor, the budget and the
    // deadline, for the reason legCommon gives about a zero interval and a zero
    // budget: a command that cannot move and cannot fail. It is also why there
    // is no no-op leg, and therefore why exit-by-flag was rejected — an exit
    // flag would have made MOTION MANDATORY FOR TEARDOWN, exactly backwards for
    // the case where you most want to bail.
    if (v[1] == 0 || v[2] == 0 || v[4] == 0 || v[5] == 0 || v[6] == 0) {
        Serial.println("err range"); return true;
    }
    return probeArmLeg((uint8_t)v[0], (uint16_t)v[1], (uint16_t)v[2],
                       (uint16_t)v[3], (uint8_t)v[4], (uint32_t)v[5],
                       (uint16_t)v[6], (uint8_t)v[7]);
}

// ── probe_end ────────────────────────────────────────────────────────────────
// "Put it back the way it was." Takes no arguments, so it works from a console
// and in the bail-out case, where the operator is already unsure what state
// things are in.
bool cmdProbeEnd(const char*) {
    return probeExit(nullptr);
}
