// place — see place.h.

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include "place.h"
#include "steps.h"
#include "../../config/machine_cfg.h"
#include "../../ops/axes_map.h"
#include "../../ops/frames.h"
#include "../../ops/machine.h"
#include "../../ops/position.h"
#include "../../ops/sequence.h"
#include "../../planner/queue.h"
#include "../../../ipc/shared_state.h"
#include "hardware/sync.h"

static bool        active, started, isSet;
static bool        leaving;           // left PLACE: the laser is off, the selection still to give back
static uint8_t     prevHead;          // the selection before the job
static OriginRange rangeX, rangeY;
static float       bbox[4];           // xmin, xmax, ymin, ymax
static bool        parking;           // the next step parks the Zs
static float       route[5][2];       // machine XY, the lines still to run from routeAt
static uint8_t     routeLen, routeAt;

// sequenceFail wants one token: `node 3 excluded` reads node_3_excluded.
static void fail(const char* why) {
    char t[40];
    snprintf(t, sizeof t, "%s", why);
    for (char* p = t; *p; p++) if (*p == ' ') *p = '_';
    sequenceFail(t);
}

static float jogFeed(void) {
    const MachineCfg& c = machineCfg();
    return fminf(c.x.jogFeed, c.y.jogFeed);
}

// Arms the next step. *why on a refusal; false with nothing left.
static bool advance(const char** why) {
    *why = nullptr;
    if (parking) {
        parking = false;
        uint8_t zs[CFG_MAX_HEADS];
        if (headZNodes(zs)) {
            *why = parkZs();
            return !*why;
        }
    }
    while (routeAt < routeLen) {
        const float* p = route[routeAt++];
        bool queued;
        *why = lineTo(p[0], p[1], jogFeed(), &queued);
        if (*why) return false;
        if (queued) return true;
    }
    return false;
}

static bool placeNext(uint8_t) {
    const char* why;
    if (advance(&why)) return true;
    if (why) fail(why);
    return false;
}

static const Sequence placeSequence = { SEQ_PLACE, placeNext, nullptr };

// Runs route[0..n) from a ring idle and empty. nullptr, or the refusal.
static const char* runRoute(uint8_t n, bool park) {
    if (!(jogFeed() > 0)) return "no_feed";
    routeLen = n;
    routeAt  = 0;
    parking  = park;
    const uint32_t s = spin_lock_blocking(plannerLock);
    ringOwner = RING_SEQ;
    spin_unlock(plannerLock, s);
    sequenceStart(&placeSequence);
    const char* why;
    if (!advance(&why) && sequenceLive()) sequenceEnd();
    return why;
}

static void routeCorner(uint8_t i, bool xmax, bool ymax) {
    route[i][0] = framesWork(SLOT_X) + bbox[xmax ? 1 : 0];
    route[i][1] = framesWork(SLOT_Y) + bbox[ymax ? 3 : 2];
}

static void openWindow(void) {
    framesDotWindow(rangeX.lo + bbox[0], rangeX.hi + bbox[0],
                    rangeY.lo + bbox[2], rangeY.hi + bbox[2]);
}

static bool selectPointer(void) {
    const MachineCfg& c = machineCfg();
    if (c.laser.node) return framesSelectLaser();
    for (uint8_t h = 0; h < c.headCount; h++) {
        if (c.heads[h].xOffset != 0.0f || c.heads[h].yOffset != 0.0f) continue;
        const char* why = nullptr;
        axesSelectHead(h, &why);
        return !why && framesSelected() == h;
    }
    return false;
}

static void start(void) {
    prevHead = framesSelected();
    isSet = false;
    if (!selectPointer()) { fail("no_pointer"); return; }
    if (const char* why = zsReady()) { fail(why); return; }
    // The zero is written only by `place set`, so the handoff is carried by the dot.
    route[0][0] = layerHandoff(framesWork(SLOT_X), rangeX) + bbox[0];
    route[0][1] = layerHandoff(framesWork(SLOT_Y), rangeY) + bbox[2];
    openWindow();
    if (const char* why = runRoute(1, true)) fail(why);
}

// The anchor before the job stays selected, its laser on; a head's selection turns it off.
static void giveBackHead(void) {
    if (prevHead >= machineCfg().headCount) return;
    if (machineCfg().laser.node) { framesSelectHead(prevHead); return; }
    const char* why = nullptr;
    axesSelectHead(prevHead, &why);
}

void placeBegin(const OriginRange& x, const OriginRange& y, const float box[4]) {
    rangeX = x;
    rangeY = y;
    for (uint8_t i = 0; i < 4; i++) bbox[i] = box[i];
    active  = true;
    started = isSet = leaving = false;
}

void placeTick(void) {
    if (!active) return;
    const bool job = machineContext == CTX_JOB;
    // A job's abort only: stopping a jog also raises abortRequested.
    if (job && jobReason == JOB_PLACE && !machineJobAborting()) {
        if (started) return;
        started = true;
        start();
        return;
    }
    if (!leaving) {
        leaving = true;
        framesDotWindowOff();
    }
    if (job) return;
    active = false;
    if (!started) return;
    if (machineAtRest()) giveBackHead();
    else framesLaserOff();   // an estop or alarm: nothing to give back to, no dot left on
}

static const char* admit(bool wantSet) {
    if (!active || leaving || machineContext != CTX_JOB || jobReason != JOB_PLACE) return "bad_state";
    if (!started || machineMoving()) return "busy";
    return isSet == wantSet ? nullptr : "bad_state";
}

const char* placeSet(void) {
    if (const char* why = admit(false)) return why;
    float x, y;
    if (!framesMPos(SLOT_X, &x) || !framesMPos(SLOT_Y, &y)) return "no_head";
    // Step rounding can put the dot just past the window.
    framesSetWork(SLOT_X, fminf(fmaxf(x - bbox[0], rangeX.lo), rangeX.hi));
    framesSetWork(SLOT_Y, fminf(fmaxf(y - bbox[2], rangeY.lo), rangeY.hi));
    framesDotWindowOff();
    isSet = true;
    return nullptr;
}

const char* placeReset(void) {
    if (const char* why = admit(true)) return why;
    routeCorner(0, false, false);
    if (const char* why = runRoute(1, false)) return why;
    openWindow();
    isSet = false;
    return nullptr;
}

const char* placeGo(bool xmax, bool ymax) {
    if (const char* why = admit(true)) return why;
    routeCorner(0, xmax, ymax);
    return runRoute(1, false);
}

const char* placeFrame(void) {
    if (const char* why = admit(true)) return why;
    routeCorner(0, false, false);
    routeCorner(1, true, false);
    routeCorner(2, true, true);
    routeCorner(3, false, true);
    routeCorner(4, false, false);
    return runRoute(5, false);
}

const char* placeOk(void) {
    if (const char* why = admit(true)) return why;
    if (!plannerJobSwap(JOB_PLACE, JOB_BUFFERING)) return "busy";
    active = false;
    giveBackHead();
    return nullptr;
}
