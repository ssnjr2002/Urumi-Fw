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
// A DTR fall closed the port, so whoever speaks next is a new session, which
// hasn't the stream: the loss stands until the job pauses or ends.
static bool     dtrHeld   = false;

// Heard now: still lost on DTR (down, or a fall held) until the next tick, so a
// `get host` in the same pass reads the host that asked. Only a silence clears:
// with DTR up throughout, the port stayed open and the speaker is the session
// that went quiet.
void hostWatchHeard(void) {
    heardMs = millis();
    if (dtrWas && !dtrHeld) hostLost = false;
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
    if (!armed || jobReason == JOB_PAUSED) dtrHeld = false;
    else if (!dtr)                         dtrHeld = true;
    const uint32_t quiet = now - heardMs < now - armedMs ? now - heardMs : now - armedMs;
    hostLost = armed && (dtrHeld || !dtr || quiet > HOST_TIMEOUT_MS);
}

uint32_t hostWatchDtrDrops(void) { return dtrDrops; }
