# Plan: Config Storage and Management (Dual Production Architecture)

**Branch:** `pipeline-redesign`  
**Date:** 2026-06-24  
**Status:** Design decided — not yet implemented

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

Every `.bin` produced by the host embeds the `MachineConfig` (and optionally
`QualityConfig`/`MotionConfig`) as a header before the packet stream. This makes
binaries self-describing and enables the Pico to validate them.

On playback the Pico reads the header, compares it against its own stored config,
and **rejects or warns** on mismatch. This is the runtime tripwire for calibration
drift — it catches the "host re-calibrated but Pico wasn't updated" failure mode.

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
- Expose via two new USB serial commands (or RS485 commands):
  - `config get` → serialise current config to TOML on stdout
  - `config set <toml>` → parse, validate (range checks), store to flash,
    echo back the stored values for confirmation

### B. Binary file config header

Extend the `.bin` file format: a header block before the first packet.

Suggested framing (fits the existing length-prefixed packet stream):

```
[magic 4B: 0x4D434647 "MCFG"] [version 1B] [payload_len 2B LE] [payload…] [CRC8 1B]
```

Payload is the same compact binary struct as the flash format. The host
serialises `MachineConfig` into this header before writing packets; the Pico
reads and validates it before accepting the stream.

`serialise_microsegments()` (or a wrapper in `svg_to_packets.py`) adds the
header. `verify_packets.py` learns to parse and display it.

### C. Host pull/push tool

`host/pico_config.py` (new script):

```
python pico_config.py pull [--port COM3] [--out machine.toml]
python pico_config.py push [--port COM3] machine.toml
python pico_config.py show [--port COM3]          # pretty-print current Pico config
```

- `pull`: sends `config get`, parses the TOML response, writes to file (or
  stdout). The output is a valid `machine.toml` the host pipeline can `load()`.
- `push`: reads `machine.toml`, sends `config set`, confirms stored values match.
- Uses the same serial connection as the existing host tools.

### D. Implement `config.load()` in `pipeline/stages/config.py`

Currently raises `NotImplementedError`. Wire it to read `machine.toml` (produced
by `pico_config.py pull`) and return a `PipelineConfig`. The `_default_machine()`
factory stays as the in-code fallback when no TOML is present.

`svg_to_packets.py` and `validate_plan.py` can then accept `--config machine.toml`
to load a pulled config in one step.

### E. Pico binary header validation

In `core0.cpp` (or a new `config.cpp`): before accepting a binary stream,
parse the MCFG header packet, compare key fields against the flash config
(steps_per_unit per axis, f_cpu). On mismatch:

- **Minor mismatch** (e.g. QualityConfig differs): warn on USB serial, proceed.
- **Major mismatch** (steps_per_unit, f_cpu): reject with an error message,
  require `--force` flag or `unalarm` to override.

---

## Implementation Order

1. **A — Pico flash storage + `config get/set` commands** (enables the rest)
2. **B — Binary file config header** (host writes it, Pico reads it)
3. **C — `pico_config.py` pull/push tool** (the operator workflow)
4. **D — `config.load()` from TOML** (closes the host side)
5. **E — Pico header validation** (the runtime enforcer)

Steps 1 and 2 are the critical path. 3 and 4 can be parallelised once 1 is done.
5 depends on 1 and 2.

---

