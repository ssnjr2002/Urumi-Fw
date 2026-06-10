# host/

Python tools that run on the PC. They consume the pipeline (stages 1–6) and communicate with the RP2350 over USB CDC serial, streaming pre-computed `MicroSegment` step events.

## Requirements

- Python 3.13+
- pyserial (`pip install pyserial` or `uv sync`)

The pipeline itself (`../pipeline/stages/`) has no external dependencies.

## Scripts

| Script | Purpose |
|---|---|
| `svg_to_packets.py` | Full pipeline: SVG → binary MicroSegment packet file |
| `sender.py` | Stream a `.bin` file to the Pico over USB CDC (Go-Back-N) |
| `jog.py` | Single ramped jog move on any axis or diagonal |
| `jog_ui.py` | Tkinter GUI for manual jogging + live machine status |
| `validate_plan.py` | Offline invariant checker — no hardware required |
| `serialise.py` | Wire-format packers/unpackers for all packet types |
| `verify_packets.py` | Validate a `.bin` packet stream without sending it |
| `test_comms.py` | Low-level comms smoke test |
| `sim_duplicates.py` | Simulate Go-Back-N duplicate behaviour |

## Typical workflow

```sh
# 1. Convert SVG to a binary packet file
python svg_to_packets.py design.svg --out job.bin

# 2. Send to the Pico
python sender.py --port COM8 --in job.bin
```

### PowerShell note

PowerShell 5 corrupts binary data in pipes between processes. Always use `--out` to route through a temp file rather than piping `svg_to_packets.py` directly into `sender.py` on Windows PowerShell.

---

## svg_to_packets.py

Runs pipeline stages 1–6 on the PC, then serialises the resulting `MicroSegment` list to a binary stream of 26-byte length-prefixed packets.

```
python svg_to_packets.py input.svg [--out file.bin] [--summary]
  --feed-max      mm/s    cruise ceiling (default 80)
  --a-max         mm/s²   acceleration (default 1000)
  --jog-feed      mm/s    travel speed between subpaths
  --lift-height   mm      Z pen lift between subpaths; 0 = draw-through (default)
  --z-feed        mm/s    Z raise/lower speed
  --tangential / --no-tangential   A-axis tracking for knife/crease vs pen
  --steps-per-mm  override XY resolution
  --steps-per-deg override A resolution
  --f-cpu         RP2350 clock Hz
  --angle-tol     C1 angle tolerance (deg)
  --gap-tol       join gap tolerance (mm)
```

`--summary` prints segment count to stderr and writes no binary output.

---

## sender.py

Validates all packets, then streams them to the Pico with a sliding-window Go-Back-N protocol.

```
python sender.py --port COM8 --in job.bin [--window 16] [-v]
```

**Protocol:** the Pico replies 3 bytes per packet:
- ACK: `[0xAA] [seq_lo] [seq_hi]`
- NACK: `[0xBB] [reason] [0x00]` — `0x01` CRC error (resend), `0x02` buffer full (backpressure), `0x03` bad magic (abort)

The sender mixes ASCII CLI text from the Pico on the same stream; framing is anchored on `0xAA`/`0xBB` so it never drifts out of sync on ASCII bytes.

Each packet carries an 8-bit rolling sequence number. After a go-back, the Pico ACKs-but-skips any packet whose seq it already consumed, preventing motion duplication on retransmit.

---

## jog.py

Generates a single straight trapezoidal move (ramp-up → cruise → ramp-down) on any axis or diagonal and streams it to the Pico.

```
python jog.py --port COM8 --axis x --dist 10           # +10 mm on X
python jog.py --port COM8 --axis y --dist -5 --feed 15 # -5 mm on Y at 15 mm/s
python jog.py --port COM8 --axis a --dist 90           # +90 deg on A
python jog.py --port COM8 --dx 10 --dy 5               # diagonal XY jog
  --feed   units/s  (default 20)
  --accel  units/s² (default 200)
  --window window size for Go-Back-N (default 16)
```

Distances are mm for linear axes (X/Y/Z) and degrees for the rotary A axis. Minor axes are Bresenham-distributed so diagonals step proportionally.

---

## jog_ui.py

Tkinter GUI providing arrow-key jogging, step-size selection, and a live status readout (state machine + dead-reckoned position). Reuses `jog.make_jog` and the `Sender`. Status polling suspends while a jog is in flight.

```
python jog_ui.py [--port COM8]
```

---

## validate_plan.py

Runs the full pipeline on an SVG and asserts six invariants offline — no hardware needed. Exit code 0 = all pass, 1 = any failure.

```
python validate_plan.py design.svg [--geom-tol 0.2] [-v]
```

| Check | What it verifies |
|---|---|
| velocity ceiling | No segment exceeds `feed_max` (drawing) or `jog_feed` (travel) |
| acceleration continuity | Estimated a = Δv²/(2d) over 0.2 mm windows ≤ `a_max` × 2 slack |
| axis rate / interval bounds | No axis exceeds `max_rate × steps_per_unit`; no interval overflow |
| step conservation | Net steps per path match geometric start→end displacement exactly |
| path boundary ramps | Every path starts and ends below 50% of its peak speed |
| geometric fidelity | Reconstructed XY trajectory deviates ≤ `geom_tol` mm from the Béziers |

---

## serialise.py

Wire-format library used by all the scripts above. Two production modes:

- **Host production** (this branch): stages 1–6 on the PC → `pack_microsegment()` → Pico executes step events directly.
- **Local production** (future): stages 1–3 on the PC → `pack_spline_tile()` → Pico runs stages 4–6 in C++.

CRC-8 polynomial `0x8C` — matches the ATtiny firmware in `include/common.h`.

### Packet formats

| Type | Magic | Size | Key fields |
|---|---|---|---|
| MicroSegment | `0xAB` | 26 B | dx, dy, dz, da (int32 LE), interval (uint32 LE), flags, seq, CRC8 |
| ToolConfig   | `0xAC` | 21 B | tool_type, feed_max, lift_kappa, lift_height, z_feed, CRC8 |
| SplineTile   | `0xAD` | 37 B | flags, 4× (x, y) float32 LE control points, CRC8 |

The binary stream framing adds a `uint16 LE` length prefix before each packet.
