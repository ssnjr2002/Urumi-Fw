# End-to-End Test Procedure — microseg-host-drive

This procedure is an **isolation ladder**: each step introduces exactly one new
unknown. If a step fails, the cause is the thing that step added — everything
below it is already proven. Do not skip rungs; the bugs hide in the gaps.

```
 1 loopback      USB framing + ACK/NACK            (no RS485, no motion)
 2 ping          RS485 command frames              (one node, 9th bit = 1)
 3 backpressure  buffer-full + sender go-back      (no motion)
 4 step (debug)  Pico→ATtiny stream path + enable  (bypasses host)
 5 line          host→Pico→motor, ONE step/segment
 6 multistep     within-segment Bresenham          (many steps, ONE packet)
 7 diagonal      minor-axis Bresenham distribution  (two axes, ONE packet)
 8 full SVG      the whole pipeline
```

---

## Preconditions — read before every session

- **Enable is required before any stream.** The ATtiny silently drops stream
  bytes unless `streamEnabled` is set via `enable <node>`.
- **`streamEnabled` resets on reflash or power-cycle.** Re-enable after either.
- **`sender.py` does NOT enable nodes** — only `test_comms.py` motion tests do.
  Enable manually before a raw `sender.py` job.
- **Stream frames have no node address.** Which motor moves is positional:

  | MicroSegment field | Node | Axis |
  |---|---|---|
  | `dx` | 1 | X |
  | `dy` | 2 | Y |
  | `dz` | 3 | Z |
  | `da` | 4 | A |

  `--axis x` drives node 1, `--axis y` drives node 2. There is no node argument
  for streaming — only `ping`/`enable`/`step` take an explicit node.
- **`steps_per_mm` must match your microstepping.** DM542 at 1/32 with a
  40 mm/rev gantry = **160 steps/mm**, not the default 80. The verifier plot
  uses the same value you pass, so "the plot looks right" does NOT prove the
  hardware config is right — only that host generation and host plotting agree.

---

## Prerequisites

- Pico 2 flashed with this branch's firmware (`pio run -e pico -t upload`).
  Startup banner over serial reads `RS485 MicroSegment Host Drive`.
- Motors on the nodes you intend to test (X=1, Y=2). Reassign a board's node ID
  with `pio run -e nodeN -t upload` if needed (compile-time `-DNODE_ID`).
- `host\.venv\Scripts\activate`, COM port known.

---

## Rung 1 — Loopback (no motion)

Isolates USB framing and the ACK/NACK protocol. Zero-step packets.

```
python host/test_comms.py --port COM8 --test loopback
```

**Pass:** 64 sent, 64 ACKed, 0 NACKs.
**If it fails:** wrong baud, or Pico not running this firmware (check banner).

---

## Rung 2 — Ping (one command frame)

Adds RS485 command frames and one live node.

```
python host/test_comms.py --port COM8 --node 2 --test ping
```

**Pass:** `Node 2: PONG`.
**If it fails:** RS485 wiring, node power, or `NODE_ID` mismatch.

---

## Rung 3 — Backpressure (no motion)

Adds buffer-full handling and the sender's go-back logic. Floods past the
512-entry ring buffer.

```
python host/test_comms.py --port COM8 --test backpressure
```

**Pass:** some NACKs received (expected — this is flow control), all 600 ACKed.
`Sent > 600` is normal: those are go-back resends, not errors.

---

## Rung 4 — Direct step debug (bypasses host path)

Adds the Pico→ATtiny **stream** path (9th bit = 0) and enable, with zero host
MicroSegment involvement. Type these over a serial console:

```
enable 2
step 2 2000
```

**Pass:** node-2 motor turns ~2 s (2000 pulses at 1000 sps).
**This rung proves the motor, wiring, and stream emission are good.** If it
works but rung 5 fails, the fault is in the host→Pico streaming, not hardware.

---

## Rung 5 — Single-step line (host streaming)

Adds the full host→Pico→motor stream path. **One step per segment** — does not
yet exercise multi-step emission.

```
python host/test_comms.py --port COM8 --test line --axis y --steps 8000 --feed 1000
```

**Pass:** all 8000 ACKed, motor moves ~50 mm (at 160 steps/mm). NACKs and
`Sent > 8000` are normal backpressure.

---

## Rung 6 — Multi-step segment ★

Adds the **within-segment Bresenham loop**. A single MicroSegment carrying many
steps. This is the cheapest test that proves the Pico emits N pulses for a
segment of N steps — the single-step line test cannot reach this.

```
python host/test_comms.py --port COM8 --test multistep --axis y --steps 8000 --feed 1000
```

**Pass:** 1 packet ACKed, motor moves the **full** distance.
**If the motor only twitches:** the major-axis stepping loop is broken — Core 1
is emitting one pulse per segment instead of `max(|dx|,|dy|,|dz|,|da|)`.

---

## Rung 7 — Diagonal (two-axis sync)

Adds minor-axis Bresenham distribution. One MicroSegment with steps on both
axes; result should be a straight diagonal.

```
python host/test_comms.py --port COM8 --test diagonal --steps 8000 --feed 1000
```

**Pass:** nodes 1 and 2 move together; motion traces a straight line, not a
stair-step or an L. (X gets 8000 pulses, Y is distributed to 6000.)

---

## Rung 8 — Full SVG pipeline

Adds the whole pipeline. Everything underneath is now proven.

### 8a — Generate and verify offline (no hardware)

```
python host/svg_to_packets.py pipeline/data/test_rect.svg --out job.bin --steps-per-mm 160
python host/verify_packets.py --in job.bin --plot --steps-per-mm 160
```

**Pass:** validator reports PASS, trajectory plot matches the SVG, net step
counts match the geometry (e.g. an 80 mm side × 160 steps/mm = 12800 steps).

### 8b — Send to hardware

```
# enable the axes the job uses, then send:
python host/sender.py --port COM8 --in job.bin
```

(Enable nodes first — `sender.py` will not.)

**Pass:** all packets ACKed, machine draws the shape at the correct size.

---

## Rung 9 — Emergency stop

While a long job runs, in a second terminal:

```
python -c "import serial; s=serial.Serial('COM8',115200); s.write(b'stop\n'); s.close()"
```

**Pass:** machine halts mid-move; Pico responds normally to the next command.

---

## Failure reference

| Symptom | Likely cause | Caught at rung |
|---|---|---|
| Banner missing / garbled | Wrong firmware or baud | 1 |
| All NACK reason 0x03 | Magic mismatch — wrong firmware | 1 |
| All NACK reason 0x01 | CRC mismatch — serialise.py / shared.h out of sync | 1 |
| Sender aborts on flood | Backpressure treated as fatal (old bug) | 3 |
| Ping timeout | RS485 wiring, node power, NODE_ID | 2 |
| No motion at all | Node not enabled (`streamEnabled` false) | 4 |
| Motor twitches, no travel | Single-pulse-per-segment — Bresenham loop missing | 6 |
| Diagonal comes out as an L | Minor-axis Bresenham wrong | 7 |
| Correct shape, half size | `steps_per_mm` mismatch (1/32 → 160) | 8 |
| Sender hangs | ACK reader not receiving — port held elsewhere | 1 |
