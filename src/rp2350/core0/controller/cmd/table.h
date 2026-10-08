#pragma once
#include "../../cmd/table.h"   // Cmd

// table.h — controller commands. They read the config, so control_plane
// answers `err unconfigured` for every one of them without a valid config.

// ─── unalarm.cpp ──────────────────────────────────────────────────────────────
bool cmdUnalarm(const char*);

// ─── home.cpp ─────────────────────────────────────────────────────────────────
bool cmdHome(const char*);
bool cmdHomeUnhomed(const char*);
bool cmdHomeCycle(const char*);
bool cmdHomeHead(const char*);

// ─── frames.cpp ───────────────────────────────────────────────────────────────
bool cmdSelect(const char*);
bool cmdWzero(const char*);
bool cmdWset(const char*);
bool cmdWclear(const char*);
bool cmdMesh(const char*);
