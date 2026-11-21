// host_watch.h — is the host still there (docs/plans/host-watch.md).
//
// While a job is open the host is lost when DTR drops (the port closed or the
// cable pulled) or nothing has arrived for HOST_TIMEOUT_MS (a host that hangs
// with the port open). Core 0 sets hostLost; Core 1 pauses a dry job on it.
#pragma once
#include <stdint.h>

// Long against the host's 100 ms poll: a background tab's timers run about
// once a second, and a false loss lifts the tool and marks the cut.
#define HOST_TIMEOUT_MS 3000

// A byte arrived from the host.
void hostWatchHeard(void);

// Once per loop: sample DTR and set hostLost.
void hostWatchTick(void);

// DTR falls since power-on (survives `reset`).
uint32_t hostWatchDtrDrops(void);
