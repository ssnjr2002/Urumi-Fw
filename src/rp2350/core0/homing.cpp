#include <Arduino.h>
#include "homing.h"
#include "position.h"
#include "../ipc/shared_state.h"
#include "../ipc/core1_rpc.h"

// Poll cadence. This does NOT set accuracy: the node's gate stops the axis at
// the trip point whatever Core 0 is doing, so the counter is exact whenever it
// is read. The cadence decides only when the Pico notices (docs/homing.md §2.3).
#define HOMING_POLL_MS      25

// Consecutive unanswered polls before the home is declared failed. One dropped
// reply on a shared 9-bit bus is not a fault; four in a row is. Kept small
// enough that a dead node is caught in ~100 ms rather than at the timeout.
#define HOMING_POLL_MISSES  4

// Ceiling on the derived timeout. A malformed budget (max_steps at its uint32
// limit with a slow floor) would otherwise compute an hours-long deadline and
// the supervisor would wait it out.
#define HOMING_TIMEOUT_CAP_MS 600000UL

// Polls a FAILING terminal reading is allowed to be re-taken before it is
// believed. There is a real window on the node between the two halves of
// stopping: the pulser ISR clears NODE_FLAG_HOMING, but homingFinish() -- which
// clears the limit latch after a successful retract -- runs in loop context on
// the next pass, because it writes flags through node_set_flag() and an ISR may
// not. A poll landing between the two reads "stopped, still asserted", which is
// exactly the signature of a retract that failed to escape.
//
// Only the failing verdict waits. Success is unambiguous and taken immediately.
#define HOMING_SETTLE_POLLS 2

static bool     claimed    = false;
static bool     wasRetract = false;
static uint8_t  hNode      = 0;
static uint8_t  misses     = 0;
static uint8_t  settleLeft = 0;
static uint32_t nextPollMs = 0;
static uint32_t deadlineMs = 0;

bool homingActive(void) { return claimed; }

// The home is over, one way or the other. Both exits invalidate the origin, and
// that is not conservatism -- it is required. A home moves the axis with the
// NODE's own pulser, so the node counts those steps and Core 1 does not; the
// machinePos it maintains is stale the moment the pulser runs. (`step` differs:
// it goes through the stream, so Core 1 adds the same steps and both frames stay
// consistent -- see cmdStep.) Dropping the datum makes that staleness visible as
// an un-homed axis instead of a plausible wrong number, and §3.4's closing
// `setorigin` is what re-derives it from the node's counter.
static void homingRelease(uint8_t node) {
    claimed = false;
    originInvalidate(node);
}

static void homingFail(void) {
    homingRelease(hNode);
    alarmReason  = ALARM_HOMING_FAIL;
    machineState = STATE_ALARM;
}

// docs/homing.md §2.3. Bounding by `max_steps × start_interval` instead would
// give a useless 88 s on X, because every step after the ramp runs at the floor.
static uint32_t homingTimeoutMs(uint16_t startUs, uint16_t floorUs,
                                uint16_t rampSteps, uint32_t maxSteps) {
    uint32_t ramp = rampSteps;
    if (ramp > maxSteps) ramp = maxSteps;
    // 64-bit throughout: 88000 steps × 65535 µs already overflows uint32.
    const uint64_t avgUs = ((uint64_t)startUs + (uint64_t)floorUs) / 2;
    uint64_t us = (uint64_t)ramp * avgUs
                + (uint64_t)(maxSteps - ramp) * (uint64_t)floorUs;
    us = us * 12 / 10;                         // §2.3's 1.2 margin
    uint64_t ms = (us / 1000) + HOMING_POLL_MS;   // never shorter than one poll
    if (ms > HOMING_TIMEOUT_CAP_MS) ms = HOMING_TIMEOUT_CAP_MS;
    return (uint32_t)ms;
}

bool homingBegin(uint8_t node, uint8_t dir, uint16_t startUs, uint16_t floorUs,
                 uint16_t rampSteps, uint32_t maxSteps) {
    NodeStatus st;
    RpcResult r = rpcHome(node, dir, startUs, floorUs, rampSteps, maxSteps, &st);
    if (r != RPC_OK) {
        // A node with no switch wired NAKs CMD_HOME, and that refusal is on the
        // wire rather than being a silent drop the master reads as absence.
        Serial.printf("err node %d %s\n", node, rpcResultText(r));
        return true;
    }
    if (!st.hasStepperTail) { Serial.println("err bad_reply"); return true; }

    // WHICH MOVE THIS IS, decided here and nowhere else. The node picks seek or
    // retract from one read of its own pin at arm time and does not report the
    // choice (docs/homing.md §1.2) -- but the ack is sampled AFTER the arm, so
    // its LIMIT bit is that very pin read, handed back in the same transaction.
    // The supervisor needs it because the terminal flags read OPPOSITELY for the
    // two modes (§1.5), and this is the only way to learn it without a second
    // poll that could straddle the switch.
    //
    // It is also why `home` carries no <seek|retract> argument: the host cannot
    // know the pin state, and the node has already answered the question.
    wasRetract = (st.flags & NODE_FLAG_LIMIT) != 0;

    // The node accepted the command but is not pulsing. This should not happen:
    // homingArm() starts TCA0 before the reply is built, the first overflow is a
    // whole start_interval away, and `home` rejects the zero budget that is the
    // only way to finish inside that window. It cannot be INTERPRETED either --
    // "stopped" and "never started" produce identical flags, so the §1.5 table
    // does not apply -- and guessing would report a home that never ran as one
    // that succeeded. Refuse, and leave the machine where it was: nothing moved,
    // so there is nothing to alarm about and no datum to drop.
    if (!(st.flags & NODE_FLAG_HOMING)) {
        Serial.printf("err node %d no_start limit %d\n", node,
                      (st.flags & NODE_FLAG_LIMIT) ? 1 : 0);
        return true;
    }

    const uint32_t now = millis();
    claimed      = true;
    hNode        = node;
    misses       = 0;
    settleLeft   = HOMING_SETTLE_POLLS;
    nextPollMs   = now + HOMING_POLL_MS;
    deadlineMs   = now + homingTimeoutMs(startUs, floorUs, rampSteps, maxSteps);
    machineState = STATE_HOMING;
    Serial.println("ok");
    return true;
}

void homingTick(void) {
    if (!claimed) return;

    // Something else has taken the machine -- `stop` sets ESTOP, Core 1 folds it
    // to ALARM. Drop the claim rather than fight for it: leaving it set would
    // fire a timeout later, at a moment with nothing to do with homing.
    if (machineState != STATE_HOMING) { homingRelease(hNode); return; }

    const uint32_t now = millis();
    if ((int32_t)(now - nextPollMs) < 0) return;
    nextPollMs = now + HOMING_POLL_MS;

    NodeStatus st;
    if (rpcNodeStatus(CMD_NODE_STATUS, hNode, 0, &st) != RPC_OK ||
        !st.hasStepperTail) {
        if (++misses >= HOMING_POLL_MISSES) homingFail();
        return;
    }
    misses = 0;

    if (st.flags & NODE_FLAG_HOMING) {
        // Still pulsing. The runaway budget is the node's, but Core 0 keeps its
        // own deadline anyway: the budget cannot catch a pulser that hangs with
        // the flag set, and the node would go on answering polls forever.
        if ((int32_t)(now - deadlineMs) >= 0) homingFail();
        return;
    }

    // Stopped. §1.5: after a seek, LIMIT set means found and clear means the
    // budget ran out without ever reaching the switch; after a retract it is the
    // other way round.
    const bool ok = wasRetract ? !(st.flags & NODE_FLAG_LIMIT)
                               :  (st.flags & NODE_FLAG_LIMIT);
    if (!ok) {
        if (settleLeft) { settleLeft--; return; }   // homingFinish() may not have run
        homingFail();
        return;
    }

    homingRelease(hNode);
    // Retire OUR OWN leftover reason, and only that one. homingFail() writes the
    // pair (ALARM, ALARM_HOMING_FAIL), but recovering the state does not
    // implicitly retire the reason, so a failed home followed by a good one used
    // to report IDLE while still naming the failure -- and the reason is what
    // the host renders, so the machine read as broken after it had recovered.
    // Anything else in there belongs to a fault this command did not cause and
    // is not ours to clear.
    if (alarmReason == ALARM_HOMING_FAIL) alarmReason = ALARM_NONE;
    machineState = STATE_IDLE;
}
