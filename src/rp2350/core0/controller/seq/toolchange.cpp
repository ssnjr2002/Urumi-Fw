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
static bool    lining;        // the line is the step

// sequenceFail wants one token: `node 3 excluded` reads node_3_excluded.
static void fail(const char* why) {
    char t[40];
    snprintf(t, sizeof t, "%s", why);
    for (char* p = t; *p; p++) if (*p == ' ') *p = '_';
    sequenceFail(t);
}

static void enterWait(void) {
    framesUnsetWorkZ(wantHead);
    if (!plannerJobSwap(JOB_TOOL_CHANGE, JOB_TOOL_WAIT)) fail("bad_state");
}

static bool toolChangeNext(uint8_t) {
    if (!lining) {
        lining = true;
        bool queued;
        if (const char* why = lineTo(targetX, targetY, feed, &queued)) {
            fail(why);
            return false;
        }
        if (queued) return true;
    }
    enterWait();
    return false;
}

static const Sequence toolChangeSequence = { SEQ_TOOL_CHANGE, toolChangeNext, nullptr };

static bool selectHead(void) {
    if (framesSelected() == wantHead) return true;
    const char* why = nullptr;
    axesSelectHead(wantHead, &why);
    if (why || framesSelected() != wantHead) { fail(why ? why : "no_head"); return false; }
    return true;
}

static void start(void) {
    if (!selectHead()) return;
    if (toolsMounted(wantHead) == wantTool) {
        if (!plannerJobSwap(JOB_TOOL_CHANGE, backTo)) fail("bad_state");
        return;
    }
    if ((axes_homed & 0x3) != 0x3) { fail("not_homed"); return; }
    if (const char* why = framesProbeFor(wantTool, &targetX, &targetY)) { fail(why); return; }
    const MachineCfg& c = machineCfg();
    feed = fminf(c.x.jogFeed, c.y.jogFeed);
    if (!(feed > 0)) { fail("no_feed"); return; }
    if (const char* why = zsReady()) { fail(why); return; }

    // The job's ring is idle and empty in TOOL_CHANGE.
    const uint32_t s = spin_lock_blocking(plannerLock);
    ringOwner = RING_SEQ;
    spin_unlock(plannerLock, s);

    uint8_t zs[CFG_MAX_HEADS];
    lining = headZNodes(zs) == 0;
    sequenceStart(&toolChangeSequence);
    if (lining) {
        // No Z to park: the line is the first step.
        bool queued;
        const char* why = lineTo(targetX, targetY, feed, &queued);
        if (!queued) sequenceEnd();
        if (why) fail(why);
        else if (!queued) enterWait();
        return;
    }
    if (const char* why = parkZs()) {
        if (sequenceLive()) sequenceEnd();
        fail(why);
    }
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

const char* toolChangeMountCheck(uint8_t head, uint8_t id) {
    if (machineContext != CTX_JOB || jobReason != JOB_TOOL_WAIT) return nullptr;
    return head == wantHead && id == wantTool ? nullptr : "wrong_tool";
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
    } else if (why == JOB_TOOL_WAIT) {
        if (toolsMounted(wantHead) == wantTool && framesWorkZSet(wantHead) &&
            !sequenceLive())
            plannerJobSwap(JOB_TOOL_WAIT, backTo);   // false while a jog runs
    } else if (!toolChangePending) {
        // Back, or a pause took the pending change.
        active = started = false;
    }
}
