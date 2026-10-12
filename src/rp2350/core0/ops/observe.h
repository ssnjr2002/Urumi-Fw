// observe.h — Core 0's observer: what Core 1 reports, folded into Core 0's view.
//
// Pops the facts Core 1 posts (ipc/core1_facts.h), each exactly once, and keeps
// the projections Core 1's writes feed (axes_enabled). Runs first in Core 0's
// loop, before anything the host can observe.

#pragma once

void observeTick(void);
