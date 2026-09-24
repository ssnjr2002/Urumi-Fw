#pragma once
#include "../../cmd/table.h"   // Cmd

// table.h — controller commands. They read the config, so control_plane
// answers `err unconfigured` for every one of them without a valid config.

// ─── unalarm.cpp ──────────────────────────────────────────────────────────────
bool cmdUnalarm(const char*);
