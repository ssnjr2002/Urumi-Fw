// probe_leg.cpp — one leg of a tool-height probe, under lockstep. See emit.h
// and docs/tool_probe.md §5.6–§5.9.
//
// The bed-floor switch is wired to the vacuum node, not to Z, so the control
// loop this file runs is closed across the RS485 bus: emit a stream byte that
// steps Z and asks the vacuum for the switch, then wait for the vacuum's answer
// before emitting the next one.
//
// WHY LOCKSTEP. The alternative — emit freely and hope the reply lands inside a
// step interval — makes the reply a hard real-time deadline against a bus that
// is also carrying the steps. Waiting for it instead makes the deadline soft:
// the Pico physically cannot emit a step it has not been answered for, so a late
// or missing reply STALLS the axis rather than colliding with the next byte.
// Every failure in this file is therefore a stop at a step count we know exactly.
//
// The cost is that the achieved feed is an OUTPUT, not a setting (§2.3): each
// poll step takes its interval plus a round trip. `ceilUs` is a ceiling on speed
// and the leg simply runs slower than it if the bus is slow.
#include <Arduino.h>
#include "emit.h"
#include "common.h"
#include "../../ipc/shared_state.h"
#include "../../ipc/core1_rpc.h"
#include "../bus/packet.h"

// How long the confirm's CMD_SWITCH_GET may take. The bus's own figure, because
// this one is a command frame on a stopped axis — nothing is moving and no depth
// is at stake, which is exactly the difference from the per-leg poll deadline.
#define PROBE_CONFIRM_TIMEOUT_MS  RESPONSE_TIMEOUT_MS

// Wait for the vacuum's stream-byte answer. Returns false on deadline.
//
// A command frame arriving here is not our answer and is skipped rather than
// mistaken for one: the reply we want is a STREAM byte (9th bit clear). Nothing
// else should be talking mid-leg, but "should" is not a decoder.
static bool awaitReply(uint16_t deadlineUs, uint8_t vacDirMask, bool* closed) {
    const uint32_t t0 = micros();
    while ((uint32_t)(micros() - t0) < deadlineUs) {
        if (!rs485.available()) continue;
        const uint16_t w = rs485.read();
        if (w & 0x100) continue;                 // command frame — not the reply
        // Fail-safe decoding, the mirror of the node's fail-safe encoding
        // (§3.3): the dir bit SET means "still closed, keep going". So a
        // corrupted byte decodes to open, and open means stop. The unsafe answer
        // is the one that has to survive the wire intact.
        *closed = (w & vacDirMask) != 0;
        return true;
    }
    return false;
}

// The contact confirm (§5.9), run with Z stationary.
//
// Two stages, and the ordering is the argument. First N zero-step stream polls:
// Z's step bit clear, the vacuum's set — the same code path as a poll step with
// one bit different, ~40 µs each, and costing NOTHING in depth because nothing
// is moving. That is what makes real statistical rejection affordable here and
// unaffordable during the descent (§4.4). Then one CMD_SWITCH_GET, because the
// stream reply carries no CRC (§3.4) and a contact declaration should not rest
// on an unchecked byte.
//
// Returns true if the switch is still open — a real contact.
static bool confirmOpen(const ProbeLegReq* rq, uint8_t vacStep, uint8_t vacDir) {
    for (uint8_t i = 0; i < rq->confirmPolls; i++) {
        rs485.flushRX();
        rs485.writeStream(vacStep);          // ask; step no axis
        bool closed = false;
        if (!awaitReply(rq->deadlineUs, vacDir, &closed)) return false;  // silence
        if (closed) return false;            // it bounced back closed — noise
    }

    // The corroborated read. A NAK or a timeout here is not a contact: the same
    // fail-safe direction as everything else in this file.
    uint8_t pkt[8] = { rq->vacNode, CMD_SWITCH_GET, 0, 0 };
    busQuiesce();
    sendPacket(pkt, 4);
    uint8_t buf[RPC_PAYLOAD_MAX];
    uint8_t gotCmd = 0;
    const uint8_t rxLen = receivePacket(rq->vacNode, CMD_SWITCH_GET, buf,
                                        PROBE_CONFIRM_TIMEOUT_MS, &gotCmd);
    if (rxLen == 0xFF || gotCmd == CMD_NAK || rxLen < 1) return false;
    return buf[0] != 0;                      // 1 = open = the surface
}

void emitProbeLeg(const ProbeLegReq* rq, ProbeLegOut* out) {
    out->cause = PROBE_OK; out->retries = 0; out->level = 0; out->steps = 0;

    if (rq->zSlot >= 4 || rq->vacSlot >= 4 || rq->pollDiv == 0) {
        out->cause = PROBE_DEADLINE;         // a malformed leg is the emitter's
        return;                              // fault, which is what DEADLINE means
    }

    const uint8_t zStep   = 1 << (rq->zSlot   * 2);
    const uint8_t zDir    = 1 << (rq->zSlot   * 2 + 1);
    const uint8_t vacStep = 1 << (rq->vacSlot * 2);
    const uint8_t vacDir  = 1 << (rq->vacSlot * 2 + 1);

    // Every byte that steps Z carries Z's dir bit, because a byte that does not
    // step an axis must not move its DIR pin (§3.2) — which is now true on the
    // node side too, so the dir bit on a non-stepping byte is simply ignored.
    const uint8_t stepByte = zStep | (rq->dir ? zDir : 0);

    const int32_t  posDelta = rq->dir ? 1 : -1;
    const uint32_t cyclesPerUs = F_CPU / 1000000u;

    // Ramp in interval space, as the node's own pulser does: shave a fixed
    // number of ticks per step from startUs down to ceilUs over rampSteps.
    // Ramping is NOT optional on a fast leg (§2.4) — Z carries a tool and an
    // unramped start at 20 mm/s loses steps, which is the one failure this whole
    // design cannot detect, because a lost step looks exactly like a shorter
    // tool.
    uint32_t intervalUs = rq->rampSteps ? rq->startUs : rq->ceilUs;
    const uint32_t shave = (rq->rampSteps && rq->startUs > rq->ceilUs)
                         ? (uint32_t)(rq->startUs - rq->ceilUs) / rq->rampSteps
                         : 0;

    while (!rs485.txEmpty());
    rs485.flushRX();
    rs485.writeStream(0);                    // NOP — resync every node's parser

    uint32_t emitted = 0;
    uint32_t sincePoll = 0;
    uint8_t  cause = PROBE_BUDGET;           // the outcome if the budget runs out
    uint32_t t0 = rp2040.getCycleCount();

    while (emitted < rq->maxSteps) {
        if (machineState == STATE_ESTOP) { cause = PROBE_ESTOP; break; }

        const uint32_t interval = intervalUs * cyclesPerUs;
        while ((rp2040.getCycleCount() - t0) < interval) {
            if (machineState == STATE_ESTOP) break;
        }
        t0 += interval;

        const bool poll = (++sincePoll >= rq->pollDiv);

        rs485.flushRX();                     // nothing stale may look like a reply
        rs485.writeStream(poll ? (uint8_t)(stepByte | vacStep) : stepByte);
        emitted++;

        if (!poll) {
            // Non-poll step: emit and carry on without waiting. The resulting
            // ripple is one round trip every pollDiv steps — irrelevant on a leg
            // that is not measuring, and the reason the approach leg can be fast.
            if (intervalUs > rq->ceilUs && shave)
                intervalUs = (intervalUs > shave + rq->ceilUs)
                           ? intervalUs - shave : rq->ceilUs;
            continue;
        }
        sincePoll = 0;

        bool closed = false;
        if (!awaitReply(rq->deadlineUs, vacDir, &closed)) {
            cause = PROBE_POLL;              // the bus, not the axis
            break;
        }

        if (intervalUs > rq->ceilUs && shave)
            intervalUs = (intervalUs > shave + rq->ceilUs)
                       ? intervalUs - shave : rq->ceilUs;

        out->level = closed ? 0 : 1;         // last thing the switch actually said
        if (closed) continue;                // still on the way down

        // ── First open. Stop here; the confirm costs no depth. ──────────────
        //
        // Trigger on the first open, immediately, unfiltered (§4.4). Rejection
        // happens below, AFTER stopping, where it is free. A filter in this loop
        // would delay every trigger including the true one, and that delay is
        // depth at exactly the moment depth is being measured.
        if (confirmOpen(rq, vacStep, vacDir)) {
            cause = PROBE_OK;
            out->level = 1;
            break;
        }

        // It went closed again: noise, not the surface. Retry means CONTINUE the
        // descent on the remaining budget, not restart the leg — the steps
        // already taken are real and Z is genuinely where the counter says.
        if (out->retries >= rq->retryLimit) {
            cause = PROBE_CHATTER;           // switch or cable noise, not depth
            break;
        }
        out->retries++;
        t0 = rp2040.getCycleCount();         // the confirm cost real time
    }

    // machinePos is advanced by what was ACTUALLY emitted, on every path
    // including estop, and Core 1 is its sole owner (§5.6). The probe runs after
    // homing, so position validity has to survive it — a leg that stops early
    // still moved the axis, and a count that ignored that would be a fiction the
    // next leg's cross-check (§5.8) would blame on the node.
    machinePos[rq->zSlot] += posDelta * (int32_t)emitted;

    out->cause = cause;
    out->steps = posDelta * (int32_t)emitted;
}
