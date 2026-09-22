# Plan: Config Storage and Management (Dual Production Architecture)

**Branch:** `pipeline-redesign`  
**Date:** 2026-06-24  
**Status:** DEFERRED — Phase 2 (local production on Pico). Not needed for Phase 1 host production.

> **Why deferred:** In Phase 1 the host bakes all machine config into the microsegments — step counts
> and intervals already encode `steps_per_unit`, `accel`, and `max_rate`. The Pico just executes what
> arrives. Config agreement between host and Pico is only meaningful when the Pico runs its own planner
> (Phase 2). Adding config sync, flash storage, and handshake commands before that point adds complexity
> with no Phase 1 benefit. Revisit when local production is scoped.

---

## Context

[Discussion](config_management_discussion.md)

The system has two microsegment production modes:

- **Host production (now):** Python pipeline on the PC generates microsegments,
  sends them to the Pico as a binary stream. All kinematic config is consumed on
  the host; the Pico emit layer just fires the step/dir bits it's handed.
- **Local production (Phase 2):** Pico runs its own planner and generates
  microsegments in RAM. Even in Phase 2 the Pico must still accept host-produced
  binaries.

The question this plan addresses: **where does config live, how does it get
there, and who enforces correctness?**

---

## Key Decisions Made

### 1. Pico is the authority

The Pico is closest to the hardware and executes the segments — it is the right
enforcer. Config is stored on Pico flash and is the single authoritative source
for what the physical machine is. The host derives its production config from the
Pico (via pull), not the other way around.

### 2. Two-location reality is accepted

Production config lives in two places: Pico flash (the authority) and whatever
the host loaded for the current job (from a pull, a file, or CLI flags). They
don't need to be kept in sync by software — they both describe the same physical
machine. Agreement is a calibration discipline, not an architectural invariant.
Drift is surfaced at runtime through the binary header validation (see §4).

### 3. Host production code is config-agnostic

`run(svg, machine, ...)` receives a `MachineConfig` and doesn't care where it
came from — a pull from the Pico, a local TOML, or a CLI override. This is
already true today. The pull workflow just becomes the canonical way to obtain
the config before a job.

### 4. Config is embedded in the binary file

Every `.bin` produced by the host embeds a config checksum (CRC32 over the
`MachineConfigFlash` struct) as a header before the packet stream. On playback
the Pico compares the header CRC32 against its own stored CRC32 — match proceeds,
mismatch rejects with `MSEG_NACK_CONFIG_MISMATCH` (see `wire_protocol.md`). No field-by-field comparison,
no major/minor split — any difference is a rejection.

The CRC32 stored on Pico flash is the same one computed and stored after a valid
`CMD_SET_CONFIG` push. `CMD_GET_CONFIG` returns the struct + this CRC32; the host
embeds that CRC32 directly in the binary header. The operator workflow on mismatch:
`pico_config pull` → diff `machine.toml` against what was used for production →
fix and re-push or re-produce.

CRC32 is used for config (stronger collision resistance than CRC8 for a
correctness gate — 1-in-4-billion false-match rate). CRC8 stays for all other
packets (MSEG, jog, etc).

### 5. Pull/push workflow replaces config file sync

No generated C headers, no shared TOML watched by both sides. Instead:

```
pico-config pull  →  machine.toml   (Pico flash → host, human-editable)
  <edit if needed>
pico-config push  ←  machine.toml   (host → Pico flash, Pico validates + stores)
```

The host uses the pulled config for production. The pull step is explicit and
operator-triggered — not automatic. `pico-config pull` is also the answer to
"what config did I run that job with?"

### 6. Local production (Phase 2) fits naturally

The Pico planner reads from its own flash directly — no pull needed, no host
involved. Config provenance is identical whether production runs on the host or
on the Pico. The binary embed/validate handshake still applies if host-produced
binaries are played back on a Pico that also has a local planner.

---

## Config Tier Split

Not all config needs to cross the host↔Pico boundary.

| Tier | Affects production output | Host needs | Pico emit needs | Pico planner needs (P2) |
|---|---|---|---|---|
| `steps_per_unit`, `f_cpu`, `invert` | yes — step math | ✓ | — | ✓ |
| `max_rate`, `accel` | yes — velocity planning | ✓ | — | ✓ |
| `QualityConfig`, `MotionConfig` | yes — intervals, feed limits | ✓ | — | ✓ |
| Node map (`AxisConfig.node`) | routing only | ✓ (informational) | ✓ authoritative | ✓ |
| `max_travel` | soft limits (deferred) | ✓ | ✓ (enforcement) | ✓ |
| `ToolProfile` | corner choreography | ✓ | — | ✓ |
| `present` | skip inactive axes | ✓ | ✓ | ✓ |

The Pico emit layer (Phase 1) only strictly needs the node map and `present`.
Everything else is production config that the host carries and embeds in the
binary.

---

## What Needs Building

### A. Pico-side config storage

- Choose flash storage: LittleFS (recommended — already available on RP2350
  Arduino core) or a fixed flash sector.
- Define a `MachineConfigFlash` C struct: compact fixed-layout binary,
  versioned with a magic number so old stored configs are detected and rejected
  rather than silently misread.
- Load on boot; fall back to hardcoded defaults if flash is blank/corrupt.
- Expose via two new binary protocol commands:
  - `CMD_GET_CONFIG` → respond with `MachineConfigFlash` binary struct + CRC32
    (the stored config checksum, used for binary header validation)
  - `CMD_SET_CONFIG` → receive struct + CRC32, validate, store struct + CRC32
    to flash. On success: ACK. On failure: NACK with reason byte:
    See `wire_protocol.md` §"Config command NACK reasons" for reason values.
    Validation failure does **not** overwrite the existing flash config.

### B. Binary file config header

Extend the `.bin` file format: a header block before the first packet.

Suggested framing:

```
[magic 4B: 0x4D434647 "MCFG"] [version 1B] [config_crc32 4B LE]
```

The `config_crc32` is the CRC32 returned by `CMD_GET_CONFIG` — the Pico's
precomputed checksum over its own `MachineConfigFlash` struct. On playback the
Pico compares this against its stored CRC32; any mismatch → reject with
`NACK_STREAM_CONFIG_MISMATCH = 0x04`. No full struct in the header, just the
checksum — keeps the header small.

`serialise_microsegments()` (or a wrapper in `svg_to_packets.py`) prepends the
header. `verify_packets.py` learns to parse and display it.

### C. Host pull/push tool

`host/pico_config.py` (new script):

```
python pico_config.py pull [--port COM3] [--out machine.toml]
python pico_config.py push [--port COM3] machine.toml
python pico_config.py show [--port COM3]          # pretty-print current Pico config
```

- `pull`: sends `CMD_GET_CONFIG`, receives binary struct, converts to
  `machine.toml` for human editing. Output is a valid TOML the pipeline
  can `load()`.
- `push`: reads `machine.toml`, converts to binary struct, sends
  `CMD_SET_CONFIG`. On NACK, prints the reason and exits non-zero — existing
  flash config is untouched. May do a best-effort pre-validation before sending
  to surface obvious errors without a round-trip.
- TOML is the human layer, lives solely on the host. Pico never sees TOML.
- Uses the same serial connection as the existing host tools.

### D. Connect-time handshake — `CMD_HANDSHAKE`

New binary command. Pico responds with a structured packet:
- Firmware version (2B)
- Config CRC32 (4B) — same value stored after `CMD_SET_CONFIG`
- `machineState` (1B)
- `axes_homed` bitmask (1B)

Host tool sends this on connect before anything else. On config CRC32 mismatch
the host warns the operator and offers to pull+diff or push. No Pico state change
on mismatch — enforcement stays at the stream header check. Pico stays in
whatever state it was in; the host decides how to resolve it.

Designed to be extensible — additional fields can be appended in future versions
without breaking older host tools (version field governs payload layout).

### F. Implement `config.load()` in `pipeline/stages/config.py`

Currently raises `NotImplementedError`. Wire it to read `machine.toml` (produced
by `pico_config.py pull`) and return a `PipelineConfig`. The `_default_machine()`
factory stays as the in-code fallback when no TOML is present.

`svg_to_packets.py` and `validate_plan.py` can then accept `--config machine.toml`
to load a pulled config in one step.

### G. Pico binary header validation

In `core0.cpp` (or a new `config.cpp`): before accepting a binary stream,
parse the MCFG header, compare `config_crc32` against the stored flash CRC32.
Any mismatch → `MSEG_NACK_CONFIG_MISMATCH`, stream rejected (see
`wire_protocol.md`). No major/minor split — any difference is a rejection.
Operator runs `pico_config pull`, diffs against the config used for production,
fixes and re-pushes or re-produces.

---

## Implementation Order

1. **A — Pico flash storage + `CMD_GET_CONFIG`/`CMD_SET_CONFIG`** (enables the rest)
2. **B — Binary file config header** (host writes it, Pico reads it)
3. **C — `pico_config.py` pull/push tool** (the operator workflow)
4. **D — `CMD_HANDSHAKE`** (connect-time config check)
5. **F — `config.load()` from TOML** (closes the host side)
6. **G — Pico header validation** (the runtime enforcer)

Steps 1 and 2 are the critical path. 3, 4, and 5 can be parallelised once 1 is
done. 6 depends on 1 and 2.

---

## Premortem — issues to resolve before implementation

### ~~1. `config get/set` over USB text protocol — fragile~~ ✓ RESOLVED
No text command. Wire protocol is `CMD_GET_CONFIG` and `CMD_SET_CONFIG` binary
packets only (fixed struct + CRC8). `pico_config.py` handles TOML↔struct
conversion on the host side. TOML is the human layer, lives solely on the host.
Pico never sees TOML.

```
pico_config.py pull  →  CMD_GET_CONFIG  →  Pico responds with binary struct
                      →  tool converts to machine.toml

pico_config.py push  ←  machine.toml
                      →  tool converts to binary struct  →  CMD_SET_CONFIG
```

### ~~2. TOML as the wire format — unnecessary round-trip~~ ✓ RESOLVED
Resolved by #1. Wire format is the compact binary struct in both directions.

**Validation layers:**
- **CRC** — transport integrity, symmetric, both directions, catches corruption.
- **Semantic validation** — Pico-side only on `CMD_SET_CONFIG`: `steps_per_unit
  == 0`, `f_cpu == 0`, node ids out of range, duplicate node ids, `max_travel
  == 0` on present axes. Pico is the authority; host tool may do a best-effort
  pre-check to avoid round-trips but Pico validation is the gate regardless.
- **On rejection** — NACK with reason byte. See `wire_protocol.md` §"Config
  command NACK reasons" for the full table. Validation failure does **not**
  overwrite existing flash config — bad push leaves old config intact. Host
  tool surfaces the reason to the operator.

### ~~3. `MachineConfigFlash` version mismatch — behaviour undefined~~ ✓ RESOLVED
Covered by the state redesign doc. Version mismatch is invalid config —
same path as any other validation failure: `ALARM_CONFIG` → `STATE_ALARM`.
See `state_redesign.md` § "Config validity — boot check and push check".

### ~~4. Major/minor mismatch split is Phase-1-specific, not durable~~ ✓ RESOLVED
No major/minor split. Binary header carries only the config CRC32 (precomputed
by Pico after a valid push, returned by `CMD_GET_CONFIG`). On playback Pico
compares CRC32 against its stored value — any mismatch is a rejection
(`MSEG_NACK_CONFIG_MISMATCH`, see `wire_protocol.md`), no exceptions. CRC32 gives negligible
false-match rate (1-in-4-billion). Struct layout divergence is prevented by
the version field. CRC32 used for config packets; CRC8 stays for all other
packets.

### ~~5. Binary header ordering not enforced at the protocol level~~ ✓ RESOLVED
Resolved by #4. The header is a compact 9-byte block (magic + version + CRC32).
If it arrives out of order or after MSEG packets, the Pico sees unrecognised
framing and rejects. The CRC32 comparison is stateless — no session gating
needed.

### ~~6. First-boot blank flash — defaults may silently disagree~~ ✓ RESOLVED
Resolved by the connect-time handshake (see §F). On connect, `CMD_HANDSHAKE`
returns the Pico's config CRC32. The host compares it against its local config
and warns or blocks before the operator produces anything. A fresh Pico with
hardcoded defaults will have a different CRC32 than any pulled config — the
mismatch surfaces immediately on connect, not silently at playback. Hardcoded
defaults are last-resort only; the handshake makes that visible.

### ~~7. `--config` not required by host tools — pull step not enforced~~ ✓ RESOLVED
Resolved by the connect-time handshake. The host tool checks config CRC32 on
connect and warns or blocks before any production runs — no need to gate on
`--config` being present or add special casing to `svg_to_packets.py`. Enforcement
is at the connect-time workflow, not the production tool.

