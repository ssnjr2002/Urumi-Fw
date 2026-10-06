// home recipe — see home.h.

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "home.h"
#include "../../config/machine_cfg.h"
#include "../../ops/homing.h"
#include "../../ops/position.h"
#include "../../ops/state.h"
#include "../../ops/bus.h"
#include "../../usb_protocol.h"       // NODE_FLAG_*, HOMING_KIND_*
#include "../../../ipc/shared_state.h"
#include "../../../ipc/core1_rpc.h"

// The re-approach budget, in back-off distances (web/src/homing/derive.ts).
#define LATCH_MARGIN  2.5
// Most the two sweeps' measured revolutions may differ, as a fraction.
#define REV_SPREAD    0.02
#define PHASES        4

enum Job : uint8_t {
    JOB_LINEAR,   // seek, back-off, re-approach, pull-off
    JOB_ROTARY,   // forward sweep, reverse sweep
    JOB_PARK,     // a homed linear node of an earlier cycle: one park leg
};

struct RunNode {
    uint8_t        node;
    const CfgAxis* ax;
    uint8_t        job;
    bool           onSwitch;   // linear: its seek was refused because it stood on
                               // its switch, so each leg runs one phase early
    int32_t        fwdIndex;   // rotary: the forward sweep's evidence
    int32_t        fwdRev;
};

static RunNode run[LEG_MAX];
static uint8_t runCount;
static bool    running;
static bool    started;        // a leg has run: a refusal now fails the session
static uint8_t cycleNow;       // 0: no run
static uint8_t phase;

static char refusal[40];

bool homeRunning(void) { return running; }

uint8_t homeAxes(const CfgAxis** out) {
    if (!machineCfgValid()) return 0;
    const MachineCfg& c = machineCfg();
    uint8_t n = 0;
    auto add = [&](const CfgAxis& a) { if (a.node.present) out[n++] = &a; };
    add(c.x);
    add(c.y);
    for (uint8_t h = 0; h < c.headCount; h++) { add(c.heads[h].z); add(c.heads[h].a); }
    return n;
}

const CfgAxis* homeAxisFor(uint8_t node) {
    const CfgAxis* axes[HOME_AXES_MAX];
    const uint8_t n = homeAxes(axes);
    for (uint8_t i = 0; i < n; i++) if (axes[i]->node.id == node) return axes[i];
    return nullptr;
}

// ── leg numbers (web/src/homing/derive.ts) ──────────────────────────────────

static uint16_t intervalUs(float feed, float spu) {
    const double us = round(1e6 / ((double)feed * (double)spu));
    return (uint16_t)(us < 1 ? 1 : us > 65535 ? 65535 : us);
}

static uint32_t distSteps(double units, float spu) {
    const double s = round(units * (double)spu);
    return s < 1 ? 1 : (uint32_t)s;
}

// A machine coordinate in wire steps: + is physical, so a wiring fix flips it.
static int32_t coordSteps(const CfgAxis& a, double units) {
    const int32_t s = (int32_t)lround(units * (double)a.stepsPerUnit);
    return a.invertDir ? -s : s;
}

// Where a linear home leaves the axis: parkPos, or the default frame (the trip
// at 0 homing −, at maxTravel + pullOffDist homing +).
static int32_t parkSteps(const CfgAxis& a) {
    const CfgHoming& h = a.homing;
    const double u = h.hasParkPos ? h.parkPos
                   : h.seekPositive ? a.maxTravel : h.pullOffDist;
    return coordSteps(a, u);
}

// Fold into (-period/2, +period/2] (derive.ts foldSigned).
static double foldSigned(double d, double period) {
    double m = fmod(fmod(d, period) + period, period);
    return m > period / 2 ? m - period : m;
}

// ── the run ─────────────────────────────────────────────────────────────────

static void runEnd(void) {
    running  = false;
    cycleNow = 0;
    homingHold(0);
}

// Arm `r`'s leg for `phase`. Sets *armed when one was armed; returns the
// refusal text, or nullptr.
static const char* armLeg(RunNode& r, bool* armed) {
    const CfgAxis&   a   = *r.ax;
    const CfgHoming& h   = a.homing;
    const float      spu = a.stepsPerUnit;
    *armed = false;
    const char* why = nullptr;
    switch (r.job) {
        case JOB_PARK:
            if (phase != 0) return nullptr;
            why = homingParkBegin(r.node, originTarget(r.node, parkSteps(a)),
                                  intervalUs(h.startFeed, spu),
                                  intervalUs(h.seekFeed, spu), h.rampSteps);
            break;
        case JOB_ROTARY:
            if (phase > 1) return nullptr;
            why = homingBegin(r.node, HOMING_KIND_INDEX, phase == 0 ? 1 : 0, false,
                              intervalUs(h.startFeed, spu),
                              intervalUs(h.sweepFeed, spu), h.rampSteps,
                              distSteps(360.0 * h.budgetRevs, spu));
            break;
        case JOB_LINEAR: {
            const uint8_t  toward = (h.seekPositive != a.invertDir) ? 1 : 0;
            const uint16_t latch  = intervalUs(h.latchFeed, spu);
            const uint32_t back   = distSteps(h.backoffDist, spu);
            uint8_t leg = phase + (r.onSwitch ? 1 : 0);
            if (leg == 0) {
                // The node's pin decides: a seek it refuses as a mismatch means
                // it stands on its switch, so its back-off runs now instead.
                bool mismatch;
                why = homingBegin(r.node, HOMING_KIND_LIMIT, toward, false,
                                  intervalUs(h.startFeed, spu),
                                  intervalUs(h.seekFeed, spu), h.rampSteps,
                                  distSteps(((double)a.maxTravel + h.pullOffDist)
                                            * h.seekScaler, spu), &mismatch);
                if (!why || !mismatch) break;
                r.onSwitch = true;
                leg = 1;
            }
            switch (leg) {
                case 1:
                    why = homingBegin(r.node, HOMING_KIND_LIMIT, !toward, true,
                                      latch, latch, 0, back);
                    break;
                case 2:
                    why = homingBegin(r.node, HOMING_KIND_LIMIT, toward, false,
                                      latch, latch, 0,
                                      (uint32_t)lround(back * LATCH_MARGIN));
                    break;
                case 3:
                    why = homingBegin(r.node, HOMING_KIND_LIMIT, !toward, true,
                                      latch, latch, 0, distSteps(h.pullOffDist, spu));
                    break;
                default:
                    break;    // leg 0 armed above; past 3, done early
            }
            if (leg > 3) return nullptr;
            break;
        }
    }
    if (!why) { *armed = true; started = true; }
    return why;
}

// Arm the current phase on every node of the cycle. Returns the refusal text
// when nothing has moved yet; after that a refusal fails the session. *any is
// set when a leg was armed.
static const char* armPhase(bool* any) {
    *any = false;
    for (uint8_t i = 0; i < runCount; i++) {
        RunNode& r = run[i];
        if (r.ax->homing.cycle != cycleNow) continue;
        bool armed;
        const char* why = armLeg(r, &armed);
        if (why) {
            if (!started) {
                snprintf(refusal, sizeof refusal, "%s", why);
                return refusal;
            }
            homingAbandon(r.node, LEGFAIL_REFUSED);
            runEnd();
            return nullptr;
        }
        if (armed) *any = true;
    }
    return nullptr;
}

// Arm the next phase that has a leg, from `phase`; *any is clear when the cycle
// has none left (or the run ended). Returns as armPhase.
static const char* armFrom(bool* any) {
    for (; phase < PHASES; phase++) {
        const char* why = armPhase(any);
        if (why || *any || !running) return why;
    }
    *any = false;
    return nullptr;
}

static void cycleStart(uint8_t c) {
    cycleNow = c;
    homingHold(c);
    phase    = 0;
}

// The lowest cycle above `c` with a run node, or 0.
static uint8_t cycleAfter(uint8_t c) {
    uint8_t next = 0;
    for (uint8_t i = 0; i < runCount; i++) {
        const uint8_t k = run[i].ax->homing.cycle;
        if (k > c && (next == 0 || k < next)) next = k;
    }
    return next;
}

// After the forward sweeps: keep each rotary node's evidence.
static bool sweepsRead(void) {
    for (uint8_t i = 0; i < runCount; i++) {
        RunNode& r = run[i];
        if (r.ax->homing.cycle != cycleNow || r.job != JOB_ROTARY) continue;
        NodeStatus st;
        if (rpcNodeStatus(CMD_NODE_STATUS, r.node, 0, &st) != RPC_OK ||
            !st.hasStepperTail) {
            homingAbandon(r.node, LEGFAIL_POLL);
            runEnd();
            return false;
        }
        r.fwdIndex = st.indexPos;
        r.fwdRev   = st.stepsPerRev;
    }
    return true;
}

// The cycle's datum: every node it homed, in one originDatum. A rotary node's
// comes from its two sweeps (derive.ts resolveRotaryIndex).
static bool cycleCommit(void) {
    uint8_t nodes[LEG_MAX];
    int32_t steps[LEG_MAX];
    uint8_t n = 0;
    for (uint8_t i = 0; i < runCount; i++) {
        const RunNode& r = run[i];
        if (r.ax->homing.cycle != cycleNow || r.job == JOB_PARK) continue;
        int32_t s;
        if (r.job == JOB_LINEAR) {
            s = parkSteps(*r.ax);
        } else {
            NodeStatus st;
            if (rpcNodeStatus(CMD_NODE_STATUS, r.node, 0, &st) != RPC_OK ||
                !st.hasStepperTail) {
                homingAbandon(r.node, LEGFAIL_POLL);
                return false;
            }
            const double rev    = (r.fwdRev + (double)st.stepsPerRev) / 2;
            const double spread = fabs((double)r.fwdRev - st.stepsPerRev);
            const double bias   = foldSigned((double)r.fwdIndex - st.indexPos, rev) / 2;
            double tol = round(r.ax->homing.toleranceDeg * r.ax->stepsPerUnit);
            if (tol < 1) tol = 1;
            if (rev <= 0 || spread > rev * REV_SPREAD || fabs(bias) > tol) {
                homingAbandon(r.node, LEGFAIL_INDEX_TOL);
                return false;
            }
            const double index = st.indexPos + bias;
            s = coordSteps(*r.ax, r.ax->homing.indexPos)
              + (int32_t)lround((double)st.pos - index);
        }
        nodes[n] = r.node;
        steps[n] = s;
        n++;
    }
    if (n == 0) return true;
    uint8_t bad;
    if (originDatum(nodes, steps, n, &bad)) {
        homingAbandon(bad, LEGFAIL_DATUM);
        return false;
    }
    return true;
}

void homeTick(void) {
    if (!running) return;
    // A failed leg alarmed, or `stop` took the machine: the run is over.
    if (machineState != STATE_HOMING) { runEnd(); return; }
    if (homingActive()) return;

    if (phase == 0 && !sweepsRead()) return;
    for (;;) {
        phase++;
        bool any;
        armFrom(&any);
        if (!running || any) return;
        // The cycle is done.
        if (!cycleCommit() || machineState != STATE_HOMING) { runEnd(); return; }
        const uint8_t next = cycleAfter(cycleNow);
        if (next == 0) {
            runEnd();
            resumeOrHold();        // IDLE, or what the legs left latched
            return;
        }
        cycleStart(next);
        armFrom(&any);
        if (!running || any) return;
        // A cycle with no leg to run leaves phase at PHASES: commit it next.
    }
}

const char* homeStart(const uint8_t* nodes, uint8_t n, bool only) {
    runCount = 0;
    uint8_t top = 0;
    for (uint8_t i = 0; i < n; i++) {
        const CfgAxis* a = homeAxisFor(nodes[i]);
        RunNode& r = run[runCount++];
        r = {};
        r.node = nodes[i];
        r.ax   = a;
        r.job  = a->rotary ? JOB_ROTARY : JOB_LINEAR;
        if (a->homing.cycle > top) top = a->homing.cycle;
    }
    // Earlier cycles are cleared first.
    if (!only) {
        const CfgAxis* axes[HOME_AXES_MAX];
        const uint8_t k = homeAxes(axes);
        for (uint8_t i = 0; i < k; i++) {
            const CfgAxis* a = axes[i];
            if (!a->homing.present || a->homing.cycle >= top) continue;
            const uint8_t id = a->node.id;
            bool named = false;
            for (uint8_t j = 0; j < n; j++) if (nodes[j] == id) named = true;
            if (named) continue;
            const bool homed = originValid(id);
            if (homed && a->rotary) continue;
            if (runCount == LEG_MAX) return "busy";
            RunNode& r = run[runCount++];
            r = {};
            r.node = id;
            r.ax   = a;
            r.job  = homed ? JOB_PARK : a->rotary ? JOB_ROTARY : JOB_LINEAR;
        }
    }
    // Every node answers before anything moves.
    for (uint8_t i = 0; i < runCount; i++) {
        const uint8_t  id  = run[i].node;
        const uint16_t bit = 1u << id;
        if (busExcluded() & bit) {
            snprintf(refusal, sizeof refusal, "node %d excluded", id); return refusal;
        }
        if (busMute() & bit) {
            snprintf(refusal, sizeof refusal, "node %d mute", id); return refusal;
        }
        const RpcResult res = rpcNodeCmd(CMD_ENABLE, id, 0);
        if (res != RPC_OK) {
            snprintf(refusal, sizeof refusal, "node %d %s", id, rpcResultText(res));
            return refusal;
        }
    }

    running = true;
    started = false;
    uint8_t first = cycleAfter(0);
    for (;;) {
        cycleStart(first);
        bool any;
        const char* why = armFrom(&any);
        if (why) { runEnd(); return why; }
        if (!running || any) return nullptr;
        // Nothing to move in this cycle (it cannot happen for a home): skip it.
        first = cycleAfter(first);
        if (first == 0) { runEnd(); return nullptr; }
    }
}
