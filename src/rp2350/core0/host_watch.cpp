// host_watch.cpp — see host_watch.h.

#include <Arduino.h>
#include "host_watch.h"
#include "../ipc/shared_state.h"

static bool     sampled   = false;
static bool     dtrWas    = false;
static uint32_t dtrDrops  = 0;
static uint32_t heardMs   = 0;
static bool     armedWas  = false;
static uint32_t armedMs   = 0;

// Heard now: lost only on DTR until the next tick, so a `get host` in the same
// pass reads the host that asked.
void hostWatchHeard(void) {
    heardMs = millis();
    if (dtrWas) hostLost = false;
}

void hostWatchTick(void) {
    const bool dtr = Serial.dtr();
    if (sampled && dtrWas && !dtr) dtrDrops++;
    dtrWas  = dtr;
    sampled = true;

    // Armed only while a job is open: the host polls steadily only then. The
    // silence counts from the job's opening at the earliest.
    const uint32_t now = millis();
    const bool armed = machineContext == CTX_JOB;
    if (armed && !armedWas) armedMs = now;
    armedWas = armed;
    const uint32_t quiet = now - heardMs < now - armedMs ? now - heardMs : now - armedMs;
    hostLost = armed && (!dtr || quiet > HOST_TIMEOUT_MS);
}

uint32_t hostWatchDtrDrops(void) { return dtrDrops; }
