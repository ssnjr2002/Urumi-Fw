// toolchange — see toolchange.h.

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "toolchange.h"
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/axes_map.h"
#include "../../ops/frames.h"
#include "../../ops/sequence.h"
#include "../../ops/tools.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"
#include "hardware/sync.h"

static bool    active;        // a header asked for a change in this job
static bool    started;       // the job reached TOOL_CHANGE and the change began
static uint8_t wantHead, wantTool;
static uint8_t backTo;        // BUFFERING or PAUSED
static float   targetX, targetY, feed;
static uint8_t step;          // the next of: park the Zs, unlock the head's A, the line

// sequenceFail wants one token: `node 3 excluded` reads node_3_excluded.
static void fail(const char* why) {
    char t[40];
    snprintf(t, sizeof t, "%s", why);
    for (char* p = t; *p; p++) if (*p == ' ') *p = '_';
    sequenceFail(t);
}

static bool ready(uint8_t head, uint8_t tool) {
    return toolsMounted(head) == tool && framesWorkZSet(head);
}

// Runs the stop's steps from `step` until one has motion out. False once the
// stop is in TOOL_WAIT, or failed.
static bool advance(void) {
    uint8_t nodes[CFG_MAX_HEADS];
    const CfgHead& hd = machineCfg().heads[wantHead];
    while (step < 3) {
        const char* why = nullptr;
        switch (step++) {
            case 0:
                if (headZNodes(nodes) == 0) continue;
                why = parkZs();
                break;
            case 1:
                if (!(hd.unlockRange > 0) || aAt(wantHead, 0)) continue;
                why = turnA(wantHead, 0);
                break;
            default: {
                bool queued;
                why = lineTo(targetX, targetY, feed, &queued);
                if (!why && !queued) continue;
                break;
            }
        }
        if (why) { fail(why); return false; }
        return true;
    }
    if (!plannerJobSwap(JOB_TOOL_CHANGE, JOB_TOOL_WAIT)) fail("bad_state");
    return false;
}

static bool toolChangeNext(uint8_t) { return advance(); }

static const Sequence toolChangeSequence = { SEQ_TOOL_CHANGE, toolChangeNext, nullptr };

static bool selectHead(uint8_t head) {
    if (framesSelected() == head) return true;
    const char* why = nullptr;
    axesSelectHead(head, &why);
    return !why && framesSelected() == head;
}

static void start(void) {
    if (!selectHead(wantHead)) { fail("no_head"); return; }
    if (ready(wantHead, wantTool)) {
        if (!plannerJobSwap(JOB_TOOL_CHANGE, backTo)) fail("bad_state");
        return;
    }
    if ((axes_homed & 0x3) != 0x3) { fail("not_homed"); return; }
    if (const char* why = framesNamed("mount", &targetX, &targetY)) { fail(why); return; }
    const MachineCfg& c = machineCfg();
    feed = fminf(c.x.jogFeed, c.y.jogFeed);
    if (!(feed > 0)) { fail("no_feed"); return; }
    if (const char* why = zsReady()) { fail(why); return; }
    if (machineCfg().heads[wantHead].unlockRange > 0)
        if (const char* why = aReady(wantHead)) { fail(why); return; }
    toolsMount(wantHead, 0);

    // The job's ring is idle and empty in TOOL_CHANGE.
    const uint32_t s = spin_lock_blocking(plannerLock);
    ringOwner = RING_SEQ;
    spin_unlock(plannerLock, s);

    step = 0;
    sequenceStart(&toolChangeSequence);
    if (!advance() && sequenceLive()) sequenceEnd();
}

bool toolChangeBegin(uint8_t head, uint8_t tool) {
    bool fromPaused;
    if (plannerHoldForTool(&fromPaused) != PQ_OK) return false;
    active   = true;
    started  = false;
    wantHead = head;
    wantTool = tool;
    backTo   = fromPaused ? JOB_PAUSED : JOB_BUFFERING;
    return true;
}

bool toolChangeHolding(void) {
    if (machineContext != CTX_JOB) return false;
    const uint8_t why = jobReason;
    return toolChangePending || why == JOB_TOOL_CHANGE || why == JOB_TOOL_WAIT;
}

bool toolChangeWaiting(void) {
    return machineContext == CTX_JOB && jobReason == JOB_TOOL_WAIT;
}

bool toolChangeLeave(uint8_t head, uint8_t tool) {
    if (!toolChangeWaiting() || sequenceLive() || !ready(head, tool)) return false;
    if (!selectHead(head)) return false;
    return plannerJobSwap(JOB_TOOL_WAIT, backTo);   // false while a jog runs
}

void toolChangeTick(void) {
    if (machineContext != CTX_JOB) {
        toolChangePending = false;
        active = started = false;
        return;
    }
    if (!active) return;
    const uint8_t why = jobReason;
    if (why == JOB_TOOL_CHANGE) {
        if (started) return;
        started = true;
        toolChangePending = false;
        start();
    } else if (why != JOB_TOOL_WAIT && !toolChangePending) {
        // Back, or a pause took the pending change.
        active = started = false;
    }
}
