#include <Arduino.h>
#include "leg.h"
#include "../../ipc/core1_rpc.h"

// Poll cadence. This does NOT set accuracy: the node's gate stops the axis at
// the trip point whatever Core 0 is doing, so the counter is exact whenever it
// is read. The cadence decides only when the Pico notices (docs/homing.md §2.3).
#define LEG_POLL_MS      25

// Consecutive unanswered polls before the leg is declared failed. One dropped
// reply on a shared 9-bit bus is not a fault; four in a row is. Kept small
// enough that a dead node is caught in ~100 ms rather than at the timeout.
#define LEG_POLL_MISSES  4

// Ceiling on the derived timeout. A malformed budget (max_steps at its uint32
// limit with a slow floor) would otherwise compute an hours-long deadline and
// the supervisor would wait it out.
#define LEG_TIMEOUT_CAP_MS 600000UL

// Polls a FAILING terminal reading is allowed to be re-taken before it is
// believed. There is a real window on the node between the two halves of
// stopping: the pulser ISR clears NODE_FLAG_LEG, but legFinish() -- which
// clears the limit latch after a successful retract -- runs in loop context on
// the next pass, because it writes flags through node_set_flag() and an ISR may
// not. A poll landing between the two reads "stopped, still asserted", which is
// exactly the signature of a retract that failed to escape.
//
// Only the failing verdict waits. Success is unambiguous and taken immediately.
#define LEG_SETTLE_POLLS 2

// The one running leg.
struct Leg {
    bool     claimed;
    // Latched at the arm from the node's declared HOMING_KIND_* and pin: a leg
    // is judged by the rules it was started under, and a node that answered a
    // poll with a different kind byte is a fault to notice, not a rule change
    // to adopt mid-flight.
    bool     rotary;
    bool     retract;
    bool     park;
    int32_t  target;    // a park's node counter
    bool     dummy;     // resolves to dummyOk at deadlineMs
    bool     dummyOk;
    uint8_t  node;
    uint8_t  misses;
    uint8_t  settleLeft;
    uint32_t nextPollMs;
    uint32_t deadlineMs;
};

static Leg legs[LEG_MAX] = {};
static uint8_t pollNext;   // round-robin start for legPoll

static Leg* legFind(uint8_t node) {
    for (Leg& g : legs)
        if (g.claimed && !g.dummy && g.node == node) return &g;
    return nullptr;
}

bool legActive(uint8_t node) { return legFind(node) != nullptr; }

bool legAny(void) {
    for (const Leg& g : legs) if (g.claimed) return true;
    return false;
}

// A free entry for `node`, or nullptr with the `err` line printed.
static Leg* legClaim(uint8_t node) {
    if (legFind(node)) { Serial.printf("err node %d busy\n", node); return nullptr; }
    for (Leg& g : legs) if (!g.claimed) return &g;
    Serial.println("err busy");
    return nullptr;
}

static void legRelease(Leg& leg, LegEnd* end, uint8_t why) {
    leg.claimed  = false;
    end->node    = leg.node;
    end->dummy   = leg.dummy;
    end->rotary  = leg.rotary;
    end->retract = leg.retract;
    end->park    = leg.park;
    end->pos     = leg.target;
    end->failWhy = why;
}

// docs/homing.md §2.3. Bounding by `max_steps × start_interval` instead would
// give a useless 88 s on X, because every step after the ramp runs at the floor.
static uint32_t legTimeoutMs(uint16_t startUs, uint16_t floorUs,
                             uint32_t rampSteps, uint32_t maxSteps) {
    uint32_t ramp = rampSteps;
    if (ramp > maxSteps) ramp = maxSteps;
    // 64-bit throughout: 88000 steps × 65535 µs already overflows uint32.
    const uint64_t avgUs = ((uint64_t)startUs + (uint64_t)floorUs) / 2;
    uint64_t us = (uint64_t)ramp * avgUs
                + (uint64_t)(maxSteps - ramp) * (uint64_t)floorUs;
    us = us * 12 / 10;                         // §2.3's 1.2 margin
    uint64_t ms = (us / 1000) + LEG_POLL_MS;   // never shorter than one poll
    if (ms > LEG_TIMEOUT_CAP_MS) ms = LEG_TIMEOUT_CAP_MS;
    return (uint32_t)ms;
}

// ROTARY_IDX_* -> LEGFAIL_*. Only ROTARY_IDX_OK is a pass, so everything that
// arrives here is a failure and the only question is what to go and look at.
//
// NOTFOUND splits on the crossing count: "the budget was too small" and "there
// is no sensor" produce the identical cause byte. Zero crossings means the
// magnet was never seen at all.
static uint8_t rotaryIdxFail(uint8_t cause, uint8_t crossings) {
    switch (cause) {
        case ROTARY_IDX_NOTFOUND:
            return crossings ? LEGFAIL_BUDGET : LEGFAIL_INDEX_ABSENT;
        case ROTARY_IDX_DEGENERATE: return LEGFAIL_INDEX_ABSENT;
        case ROTARY_IDX_OVERFLOW:   return LEGFAIL_INDEX_SHAPE;
        case ROTARY_IDX_SLIP:       return LEGFAIL_INDEX_SLIP;
        // ROTARY_IDX_NONE after a completed leg means resolve never ran at all.
        // That is the node failing to answer, not the mechanism failing to move,
        // which is exactly what POLL already names.
        case ROTARY_IDX_NONE:       return LEGFAIL_POLL;
        default:                    return LEGFAIL_POLL;   // cause we do not know
    }
}

bool legArm(uint8_t node, uint8_t expectKind, uint8_t dir, bool intendedRetract,
            uint16_t startUs, uint16_t floorUs, uint16_t rampSteps, uint32_t maxSteps) {
    Leg* lp = legClaim(node);
    if (!lp) return false;
    Leg& leg = *lp;

    // ASK WHAT THE NODE IS BEFORE ARMING IT. The kind also arrives in the arm
    // ack below, but that ack is sampled AFTER the pulser has started: a
    // `leg <n> seek` aimed at a rotary node would run a full sweep before anyone
    // noticed. One extra transaction, ~1 ms, buys a refusal that costs no
    // motion. A node with no terminator (HOMING_KIND_NONE) fails here too, and
    // the error names what the node IS.
    NodeStatus probe;
    RpcResult pr = rpcNodeStatus(CMD_NODE_STATUS, node, 0, &probe);
    if (pr != RPC_OK) {
        Serial.printf("err node %d %s\n", node, rpcResultText(pr));
        return false;
    }
    if (!probe.hasStepperTail) { Serial.println("err bad_reply"); return false; }
    if (probe.homingKind != expectKind) {
        Serial.printf("err kind_mismatch node %d is %d want %d\n",
                      node, probe.homingKind, expectKind);
        return false;
    }

    NodeStatus st;
    RpcResult r = rpcHomeLeg(node, dir, intendedRetract, startUs, floorUs,
                             rampSteps, maxSteps, &st);
    if (r != RPC_OK) {
        // A node with no switch wired NAKs CMD_HOME_LEG, and that refusal is on
        // the wire rather than being a silent drop the master reads as absence.
        // Same path covers NAK_INTENT_MISMATCH: the host's plan disagreed with
        // the node's own switch read (docs/homing.md §1.4/§2.6).
        Serial.printf("err node %d %s\n", node, rpcResultText(r));
        return false;
    }
    if (!st.hasStepperTail) { Serial.println("err bad_reply"); return false; }

    // WHICH MOVE THIS IS, decided here and nowhere else. The node picks seek or
    // retract from one read of its own pin at arm time and does not report the
    // choice (docs/homing.md §1.2) -- but the ack is sampled AFTER the arm, so
    // its LIMIT bit is that very pin read. The terminal flags read OPPOSITELY
    // for the two modes (§1.5), and this is the only way to learn it without a
    // second poll that could straddle the switch.
    //
    // A rotary node has no pin, so its LIMIT bit is permanently 0 and would
    // classify every sweep as a seek. Kind rides in this same ack, so read it
    // here rather than trusting the probe above, a whole round trip older.
    const bool rotary = (st.homingKind == HOMING_KIND_INDEX);

    // The node accepted the command but is not pulsing. legArm() on the node
    // starts TCA0 before the reply is built and the first overflow is a whole
    // start_interval away, so this should not happen. It cannot be INTERPRETED
    // either -- "stopped" and "never started" produce identical flags -- so
    // refuse: nothing moved, so there is nothing to alarm about.
    if (!(st.flags & NODE_FLAG_LEG)) {
        Serial.printf("err node %d no_start limit %d\n", node,
                      (st.flags & NODE_FLAG_LIMIT) ? 1 : 0);
        return false;
    }

    const uint32_t now = millis();
    leg.claimed    = true;
    leg.rotary     = rotary;
    leg.retract    = !rotary && (st.flags & NODE_FLAG_LIMIT) != 0;
    leg.park       = false;
    leg.dummy      = false;
    leg.node       = node;
    leg.misses     = 0;
    leg.settleLeft = LEG_SETTLE_POLLS;
    leg.nextPollMs = now + LEG_POLL_MS;
    leg.deadlineMs = now + legTimeoutMs(startUs, floorUs, rampSteps, maxSteps);
    return true;
}

bool legArmPark(uint8_t node, int32_t target, uint16_t startUs,
                uint16_t floorUs, uint16_t rampSteps) {
    Leg* lp = legClaim(node);
    if (!lp) return false;
    Leg& leg = *lp;

    // The kind probe, as legArm: a node with no terminator has no datum to
    // park by, and the error names what it is.
    NodeStatus probe;
    RpcResult pr = rpcNodeStatus(CMD_NODE_STATUS, node, 0, &probe);
    if (pr != RPC_OK) {
        Serial.printf("err node %d %s\n", node, rpcResultText(pr));
        return false;
    }
    if (!probe.hasStepperTail) { Serial.println("err bad_reply"); return false; }
    if (probe.homingKind == HOMING_KIND_NONE) {
        Serial.printf("err kind_mismatch node %d is %d\n", node, probe.homingKind);
        return false;
    }

    NodeStatus st;
    RpcResult r = rpcParkLeg(node, target, startUs, floorUs, rampSteps, &st);
    if (r != RPC_OK) {
        // NAK_NO_DATUM, NAK_INTENT_MISMATCH (on the switch), NAK_BUSY: nothing
        // moved.
        Serial.printf("err node %d %s\n", node, rpcResultText(r));
        return false;
    }
    if (!st.hasStepperTail) { Serial.println("err bad_reply"); return false; }

    // Distance from the ack, which is sampled after the arm: the counter may
    // have moved a step or two, which only lengthens the deadline.
    const int64_t  d     = (int64_t)target - (int64_t)st.pos;
    const uint32_t steps = (uint32_t)(d < 0 ? -d : d) + 1;
    // Ramped at both ends.
    const uint32_t ramp  = 2u * rampSteps;

    const uint32_t now = millis();
    leg.claimed    = true;
    leg.rotary     = (st.homingKind == HOMING_KIND_INDEX);
    leg.retract    = false;
    leg.park       = true;
    leg.target     = target;
    leg.dummy      = false;
    leg.node       = node;
    leg.misses     = 0;
    leg.settleLeft = 0;
    leg.nextPollMs = now + LEG_POLL_MS;
    leg.deadlineMs = now + legTimeoutMs(startUs, floorUs, ramp, steps);
    return true;
}

bool legAbort(uint8_t node, LegEnd* end, bool* released) {
    *released = false;
    NodeStatus st;
    RpcResult r = rpcLegAbort(node, &st);
    if (r != RPC_OK) {
        Serial.printf("err node %d %s\n", node, rpcResultText(r));
        return false;
    }
    if (Leg* g = legFind(node)) {
        legRelease(*g, end, LEGFAIL_ABORTED);
        *released = true;
    }
    return true;
}

uint8_t legAbortAll(LegEnd* ends) {
    uint8_t n = 0;
    for (Leg& g : legs) {
        if (!g.claimed) continue;
        if (g.dummy) { legRelease(g, &ends[n++], LEGFAIL_ABORTED); continue; }
        NodeStatus st;
        const bool acked = rpcLegAbort(g.node, &st) == RPC_OK && st.hasStepperTail;
        const bool kept  = acked && g.park && (st.flags & NODE_FLAG_DATUM);
        legRelease(g, &ends[n], kept ? LEGFAIL_NONE : LEGFAIL_ABORTED);
        if (kept) ends[n].pos = st.pos;
        n++;
    }
    return n;
}

bool legArmDummy(bool succeed, uint32_t ms) {
    Leg* lp = nullptr;
    for (Leg& g : legs) if (!g.claimed) { lp = &g; break; }
    if (!lp) { Serial.println("err busy"); return false; }
    Leg& leg = *lp;
    leg.claimed    = true;
    leg.rotary     = false;
    leg.retract    = false;
    leg.park       = false;
    leg.dummy      = true;
    leg.dummyOk    = succeed;
    leg.node       = 0;
    leg.deadlineMs = millis() + ms;
    return true;
}

uint8_t legDropAll(LegEnd* ends) {
    uint8_t n = 0;
    for (Leg& g : legs)
        if (g.claimed) legRelease(g, &ends[n++], LEGFAIL_NONE);
    return n;
}

static LegPoll legPollOne(Leg& leg, LegEnd* end, uint32_t now);

LegPoll legPoll(LegEnd* end) {
    const uint32_t now = millis();
    for (uint8_t i = 0; i < LEG_MAX; i++) {
        Leg& g = legs[(pollNext + i) % LEG_MAX];
        if (!g.claimed) continue;
        // A dummy is due at its deadline, a node leg at its next poll.
        const uint32_t due = g.dummy ? g.deadlineMs : g.nextPollMs;
        if ((int32_t)(now - due) < 0) continue;
        // One bus poll per call, so the next call starts after this leg.
        pollNext = (uint8_t)((pollNext + i + 1) % LEG_MAX);
        return legPollOne(g, end, now);
    }
    return LEG_RUNNING;
}

static LegPoll legPollOne(Leg& leg, LegEnd* end, uint32_t now) {
    if (leg.dummy) {
        if ((int32_t)(now - leg.deadlineMs) < 0) return LEG_RUNNING;
        if (!leg.dummyOk) { legRelease(leg, end, LEGFAIL_DUMMY); return LEG_FAILED; }
        legRelease(leg, end, LEGFAIL_NONE);
        return LEG_DONE;
    }
    if ((int32_t)(now - leg.nextPollMs) < 0) return LEG_RUNNING;
    leg.nextPollMs = now + LEG_POLL_MS;

    NodeStatus st;
    if (rpcNodeStatus(CMD_NODE_STATUS, leg.node, 0, &st) != RPC_OK ||
        !st.hasStepperTail) {
        if (++leg.misses >= LEG_POLL_MISSES) {
            legRelease(leg, end, LEGFAIL_POLL);
            return LEG_FAILED;
        }
        return LEG_RUNNING;
    }
    leg.misses = 0;

    if (st.flags & NODE_FLAG_LEG) {
        // Still pulsing. The runaway budget is the node's, but Core 0 keeps its
        // own deadline anyway: the budget cannot catch a pulser that hangs with
        // the flag set, and the node would go on answering polls forever.
        if ((int32_t)(now - leg.deadlineMs) >= 0) {
            legRelease(leg, end, LEGFAIL_DEADLINE);
            return LEG_FAILED;
        }
        return LEG_RUNNING;
    }

    // Stopped. The kinds of leg are ended by different things and there is
    // no flag they share.
    if (leg.park) {
        // The target is absolute, so where it stopped is the whole verdict. A
        // switch on the way, or a reset (which clears the datum and the
        // counter), both land elsewhere or drop the witness.
        const bool ok = st.pos == leg.target &&
                        (st.flags & NODE_FLAG_DATUM) &&
                        (leg.rotary || !(st.flags & NODE_FLAG_LIMIT));
        if (!ok) { legRelease(leg, end, LEGFAIL_PARK); return LEG_FAILED; }
    } else if (leg.rotary) {
        // No settle window: the node resolves the index BEFORE it publishes
        // NODE_FLAG_LEG clear (stepper.cpp node_loop), so the cause is final by
        // the time this poll can see the leg stopped.
        if (st.indexCause != ROTARY_IDX_OK) {
            legRelease(leg, end, rotaryIdxFail(st.indexCause, st.crossings));
            return LEG_FAILED;
        }
    } else {
        // §1.5: after a seek, LIMIT set means found and clear means the budget
        // ran out without ever reaching the switch; after a retract it is the
        // other way round.
        const bool ok = leg.retract ? !(st.flags & NODE_FLAG_LIMIT)
                                    :  (st.flags & NODE_FLAG_LIMIT);
        if (!ok) {
            if (leg.settleLeft) { leg.settleLeft--; return LEG_RUNNING; }  // legFinish() may not have run
            legRelease(leg, end, LEGFAIL_BUDGET);
            return LEG_FAILED;
        }
    }

    legRelease(leg, end, LEGFAIL_NONE);
    return LEG_DONE;
}
