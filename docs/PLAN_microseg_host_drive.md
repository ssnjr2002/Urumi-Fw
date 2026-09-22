# Plan: microseg-host-drive

**Branch:** `microseg-host-drive` (from `motion-plan`)  
**Date:** 2026-06-07  
**Status:** In progress

---

## Goal

Run the machine from a host PC that generates MicroSegments. The Pico is a dumb step emitter — no onboard planning. This validates the Python pipeline on real hardware and gives us a contingency mode that works independently of the future onboard planner.

---

## Production Modes

Two modes share the same Pico firmware and wire transport, distinguished by magic byte:

| Mode | Host runs | Pico runs | Magic |
|---|---|---|---|
| **Host production** (this branch) | Pipeline stages 1–6 | Step emitter only | `0xAB` MicroSegment |
| **Local production** (future `svg-tile-motion`) | Pipeline stages 1–3 | Stages 4–6 in C++ | `0xAD` SplineTile, `0xAC` ToolConfig |

Both modes can coexist on the same USB stream. The Pico dispatches on magic byte.

---

## Wire Packet Formats

### MicroSegment (26 bytes, magic `0xAB`)
```
[0]      magic = 0xAB
[1..4]   dx       int32 LE   X steps (signed)
[5..8]   dy       int32 LE   Y steps (signed)
[9..12]  dz       int32 LE   Z steps (signed)
[13..16] da       int32 LE   A steps (signed)
[17..20] interval uint32 LE  RP2350 CPU cycles between steps
[21]     flags    uint8      0x01=PATH_END  0x02=ESTOP
[22..24] pad      3 bytes    zero
[25]     CRC8 over bytes [0..24]
```

### SplineTile (37 bytes, magic `0xAD`) — local production, future
### ToolConfig (21 bytes, magic `0xAC`) — local production, future

### ACK / NACK (Pico → Host, 3 bytes each)
```
ACK:  [0xAA] [seq_lo] [seq_hi]
NACK: [0xBB] [reason] [0x00]
  0x01 = CRC error
  0x02 = buffer full
  0x03 = bad magic
```

---

## Architecture

```
Host PC                          Pico
──────────────────────────────   ──────────────────────────────
SVG → stages 1–6                 Core 0: binary ingest + ACK/NACK
         │                               text commands (ping/enable/etc.)
         ▼                               │
host/serialise.py                        ▼
         │                       MicroSegment ring buffer (512 entries)
         ▼                               │
host/sender.py  ── USB CDC ──►   Core 1: pop → pack stream byte → wait interval
                ◄── ACK/NACK ─           │
                                         ▼
                                 RS485 → ATtiny nodes
```

---

## File Layout

```
host/
  serialise.py      — all wire packers (MicroSegment + SplineTile + ToolConfig)
  sender.py         — windowed ACK/NACK serial sender
  svg_to_packets.py — CLI entry point: SVG → packets via full pipeline
  verify_packets.py — offline validator + trajectory plot
  main.py           — top-level CLI (TBD)

pipeline/stages/    — pure transforms, no wire knowledge (unchanged)
src/rp2350/
  shared.h          — MicroSegment struct, ring buffer, magic byte constants
  core0.cpp         — binary ingest state machine + text command handler
  core1.cpp         — MicroSegment consumer + RS485 emitter
```

---

## Build Order

1. ✅ Firmware — `MicroSegment` struct, Core 0 ingest, Core 1 consumer
2. ✅ `host/serialise.py` — wire packers for all packet types
3. ✅ `host/svg_to_packets.py` — full pipeline CLI
4. ☐ `host/verify_packets.py` — update for MicroSegment packets, trajectory plot
5. ☐ `host/sender.py` — windowed ACK/NACK sender (window=16, retry on NACK)
6. ☐ End-to-end test — synthetic straight line, then simple SVG on real hardware

---

## Notes

- Stage 7 no longer exists as a pipeline stage. Serialisation lives in `host/serialise.py`.
- SplineTile magic changed from `0xAB` → `0xAD` to avoid collision with MicroSegments.
- Machine state and tool state are deferred to a follow-on layer on this branch once comms are verified.
- The Python pipeline (stages 1–6) becomes the ground-truth reference when the local production branch ports stages 4–6 to C++.
