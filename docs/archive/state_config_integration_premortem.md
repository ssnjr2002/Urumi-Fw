# Premortem: State × Config Integration

**Branch:** `pipeline-redesign`
**Date:** 2026-06-27
**Status:** DEFERRED — Phase 2. All issues here are at the state×config seam, which does not exist in Phase 1 (config never reaches Pico; Pico only executes pre-baked microsegments).

---

## Purpose

[State redesign](state_redesign.md) and [config management](PLAN_config_management.md)
were premortemed separately, and each is internally consistent. This doc hunts
the **seams between them** — places where a decision in one design has an
unstated consequence in the other. These are the bugs that survive single-design
review because no one owns the boundary.

The two designs share three touch points:
- `ALARM_CONFIG` / `STATE_ALARM` (config validity → state machine)
- `axisBounds` populated from config (config values → position model)
- `CMD_SET_CONFIG` / `CMD_HANDSHAKE` / stream header check (config commands run
  against a live state machine on two cores)

---

## Issues

### ~~1. Flash write vs Core 1 timing — the hardware seam~~ ✓ RESOLVED
`CMD_SET_CONFIG` blocked in RUNNING, PAUSED, and HOMING — RP2350 flash write
stalls XIP for both cores, disrupting Core 1 step timing. Allowed only in IDLE
and ALARM (the config-recovery path). Rejected with `NACK_CONFIG_BAD_STATE`.
See `wire_protocol.md` §"Command Allowed-State Matrix" and §"Config command
NACK reasons".

---

### ~~2. `MSEG_NACK_CONFIG_MISMATCH` — state transition undefined~~ ✓ RESOLVED
Stay IDLE + NACK. A config CRC32 mismatch is a host-side "wrong binary for this
machine" problem, not a machine fault — ALARM would conflate it with hardware
faults that need physical intervention. Host gets `MSEG_NACK_CONFIG_MISMATCH`,
fixes the config or re-produces the binary, retries. The connect-time handshake
(issue #7) catches this earlier anyway. `machineState` unchanged.

---

### ~~3. Config push during PAUSE — guard missing from config doc~~ ✓ RESOLVED
Covered by #1's resolution. `CMD_SET_CONFIG` blocked in PAUSED per the
command/state matrix in `wire_protocol.md`. Config doc section A now points
to `wire_protocol.md` for the full state guard — no separate mention needed.

---

### 4. One job header or two? — `required_axes` vs config CRC32 (consistency)

The state doc embeds `required_axes` (the tool mask) in "the binary job header."
The config doc embeds `config_crc32` in the "MCFG header." Are these the same
header with two fields, or two separate framed blocks at the top of the `.bin`?

Never reconciled. If they're separate, the Pico must parse two preamble blocks
in a defined order before the first MSEG; if they're one, the framing in config
doc §B needs a `required_axes` field added.

**Resolution direction:** one preamble block. Extend the MCFG header to carry
both `config_crc32` and `required_axes` (and the struct version that governs
layout). Single parse, single ordering rule, single rejection point.

---

### 5. Pushing config mutates `axisBounds` — may strand a homed position (correctness)

The state doc says `axisBounds` is repopulated from config on every
`CMD_SET_CONFIG`. But if the operator pushes a config with a smaller `max_travel`
or a different `steps_per_unit` while axes are homed:

- `axisBounds` shrinks/shifts, but `machinePos` (in steps) is unchanged — the
  current position may now be **out of the new bounds**, or
- `steps_per_unit` changes, so `machinePos` in steps now means a *different
  physical location* than before the push.

`axes_homed` would still read "homed" while the position model is silently
inconsistent. This is the calibration-discipline hazard made concrete by a
mid-session push.

**Resolution direction:** a successful `CMD_SET_CONFIG` that changes any
production-involved axis field clears `axes_homed` and resets `axisBounds` to
the not-homed sentinel — same as a `disable`. Position must be re-established
after a config change. (Pushing an *identical* config — same CRC32 — is a no-op
and need not clear homing.) Since #1 already restricts push to IDLE/ALARM, no
job is in flight when this happens.

---

### 6. Config-command allowed-state matrix never written down (implementation gap)

Across both docs, config-related commands have implied but never tabulated state
preconditions. The recovery path *requires* `CMD_SET_CONFIG` to work during
`STATE_ALARM` (boot with invalid config → `ALARM_CONFIG` → push valid config →
`unalarm`), yet #1 wants it blocked in RUNNING/PAUSED. `CMD_GET_CONFIG` and
`CMD_HANDSHAKE` are read-only and should work in any state. Without a single
table, an implementer will guess inconsistently.

**Resolution direction:** publish one matrix (proposed):

| Command | IDLE | RUNNING | PAUSED | ALARM | HOMING |
|---|---|---|---|---|---|
| `CMD_HANDSHAKE` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `CMD_GET_CONFIG` | ✓ | ✓ | ✓ | ✓ | ✓ |
| `CMD_SET_CONFIG` | ✓ | ✗ | ✗ | ✓ | ✗ |
| stream / MSEG | ✓ | ✓ (enqueue) | ✗ | ✗ | ✗ |

`CMD_SET_CONFIG` in ALARM is the recovery path; it does not require the alarm to
be `ALARM_CONFIG` specifically (any alarm is a safe, stopped state to recalibrate
from), which is slightly looser than #1 — decide whether to allow it in all
alarms or only `ALARM_CONFIG`.

---

### 7. Handshake vs stream-check — two answers to "config mismatch" (consistency, low risk)

The handshake (config doc §D) reports a CRC32 mismatch but deliberately does
*not* change state — the host decides. The stream header check (§G) *rejects*
on the same logical condition. This is intentional (advisory vs enforcing) and
correct, but the docs never state that the two are the same comparison
(`header/handshake CRC32` vs `stored flash CRC32`) evaluated at two different
moments. Worth one explicit sentence so they can't drift apart in implementation
(e.g. one using CRC32-of-struct and the other CRC32-of-something-else).

**Resolution direction:** document that both use the identical stored flash
CRC32 as the reference, and the same CRC32 algorithm/region. No behavioural
change.

---

### 8. Cross-core reads in the handshake (low risk, worth a note)

`CMD_HANDSHAKE` runs on Core 0 and reads `machineState` and `axes_homed`, both
owned (for writes) under the state doc's ownership discipline. 8-bit reads are
atomic on the RP2350 so there's no torn-read hazard, but the returned snapshot
can be stale by the time the host acts on it (Core 1 may transition immediately
after). This is fine for an advisory handshake — just don't let any *enforcing*
logic depend on the handshake's state fields being current. Enforcement stays at
the per-emit / per-enqueue checks, which read state at the authoritative moment.

---

## Summary

| # | Seam | Severity |
|---|---|---|
| 1 | Flash write stalls Core 1 — push must be IDLE/ALARM only | critical |
| 2 | Stream config-mismatch state transition undefined | correctness |
| 3 | Config push during PAUSE guard missing from config doc | correctness |
| 4 | One job header or two (required_axes + config CRC32) | consistency |
| 5 | Config push may strand a homed position | correctness |
| 6 | Config-command allowed-state matrix unwritten | impl gap |
| 7 | Handshake vs stream-check use same comparison | consistency |
| 8 | Handshake cross-core reads are snapshots | low |

Issues 1, 5, and 6 are the load-bearing ones — they change firmware behaviour.
2, 3, 4, 7 are mostly reconciliation between the two docs. 8 is a note.
