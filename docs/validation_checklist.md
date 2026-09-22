# Validation checklist — host, Pico, nodes

**Purpose:** establish, by elimination, which of three segments a fault lives in.
Every item states what a **failure** rules out, because that is what makes this a
diagnostic procedure rather than a list of things that happened to work.

## The three domains

| | Segment | Driven by | Nodes attached? |
|---|---|---|---|
| **A** | host ↔ Pico | Python (`host/`) | No |
| **B** | Pico ↔ nodes | USB terminal only, no Python | Yes |
| **C** | host ↔ Pico ↔ nodes | Python | Yes |

**Run order: S → B → A → C.** A and B are independent; each is a smaller search
space than C. Do not debug in C what you have not first isolated.

### The diagnosis rule

| S | A | B | C | Fault is in |
|---|---|---|---|---|
| ✗ | — | — | — | Host logic. Fix before touching hardware. |
| ✓ | ✗ | ✓ | ✗ | USB link, framing, seq/ACK, or Core 0 |
| ✓ | ✓ | ✗ | ✗ | RS485 bus, node firmware, wiring, or termination |
| ✓ | ✓ | ✓ | ✗ | **Translation** — units, `steps_per_unit`, `invert`, planner geometry, Core 1 kinematics |

That last row is the one worth the whole exercise. A✓ B✓ C✗ is a completely
different search space from "the bus is flaky", and the two are indistinguishable
without testing the segments separately.

---

## Before anything

- [ ] **Motors unloaded and free to turn.** Every direction and step-rate item
      below can move an axis unexpectedly.
- [ ] **ESTOP reachable** — physical cutoff, not just the UI button. A wrong
      `steps_per_unit` by 10× is a crash, not a wrong number.
- [ ] **Know your travel limits by hand.** `rampStepInBounds()` is a
      pass-through stub (see *Known gaps*); nothing in firmware will stop you.
- [ ] Firmware flashed from the current tree: `pio run -e pico -t upload`.
- [ ] Only one thing owns the COM port. The Python `Link` holds it for the whole
      connection; a terminal and a script cannot both be attached.

---

## S — Host only (simulator)

No hardware. Run first so a red suite is never blamed on wiring.

- [ ] **S1 — suites green.**
      `python -m host.diagnostics.test_protocol` (and `test_session`, `test_reader`,
      `test_ui_jog`, `test_job_runner`, `test_planner`).
      **Expect:** `all passed` / `N passed, 0 failed`.
      ⚠️ **These harnesses exit 0 even when they fail.** Read the verdict block,
      or `grep -c FAIL`. Do not trust `tail`, and do not trust `$?`.
      **Failure eliminates:** nothing about hardware. Stop and fix here.

- [ ] **S2 — plan validation.** `python -m host.production.validate_plan <svg>`
      **Expect:** velocity ceilings, interval bounds and closure checks pass.
      **Failure eliminates:** the machine. It's planner or config.

- [ ] **S3 — packet inspection.** `python -m host.production.verify_packets <plan.bin>`
      **Expect:** framing intact, CRCs valid, path breaks present (non-zero — a
      zero count is the signature of the `MSEG_FLAG_PATH_END` regression).

---

## B — Pico ↔ nodes (terminal only)

**No Python.** Any serial terminal at 115200. This is the half of the system the
host cannot reach past, so proving it standalone is what makes A✓B✓C✗ meaningful.

- [ ] **B1 — Pico alive.** `ping` → `pong`. `status` → human-readable dump.
      **Failure eliminates:** everything downstream. Fix the board/flash first.

- [ ] **B2 — every node answers.** `pingnode all`
      **Expect:** one line, `nodes 1=ok 2=ok …` for every node physically present.
      **Failure (all nodes):** bus-wide — wiring, termination, DE pin, baud, ground.
      **Failure (one node):** that node — `NODE_ID` build flag, its transceiver, its power.
      *This is the single highest-value check in the document.*

- [ ] **B3 — addressing is not aliased.** `pingnode 1`, `pingnode 2`, … individually.
      **Expect:** each answers, and unpopulated IDs time out.
      **Failure:** two nodes flashed with the same `NODE_ID` — they will answer
      as one and corrupt each other's motion later in a way that looks like lost steps.

- [ ] **B4 — enable reaches the driver.** `enable 1`
      **Expect:** motor audibly/physically holds (EN is active LOW).
      **Failure:** EN wiring or the node's pin config, not the bus (B2 passed).

- [ ] **B5 — one node steps.** `step 1 200`
      **Expect:** shaft moves; `getpos` advances by exactly 200.
      **Failure with B4 ✓:** node step/dir output or the driver, not addressing.

- [ ] **B6 — direction is symmetric.** `step 1 200` then `step 1 -200`
      **Expect:** returns to the same physical spot; `getpos` back to start.
      **Failure:** DIR setup-time violation or a driver latching direction late.

- [ ] **B7 — position survives.** `setorigin`, `step 1 1000`, `getpos`
      **Expect:** exactly 1000. Repeat ×5 — the counter must not drift.
      **Failure:** the node's ISR is losing or double-counting step events.

- [ ] **B8 — steps per unit, measured.** `setorigin`, `step 1 <steps_per_unit×10>`,
      then measure the physical travel with a rule or indicator.
      **Expect:** 10 mm (or 10° on A) within your measurement error.
      **Failure:** the config is wrong, and every geometry result in C is
      meaningless until it's fixed. **Do this before trusting any job output.**

- [ ] **B9 — direction ground truth.** `setorigin`, `step 1 500`, note which way
      the axis physically moves.
      **Record it.** `step` is raw motor steps — no planner, no config, no
      `invert` — so this is the reference every later direction claim is checked
      against.

Repeat B4–B9 per node.

---

## A — Host ↔ Pico (nodes disconnected)

Everything here is bus-independent. This is what all runs to date have exercised.

- [ ] **A1 — link opens clean.** Connect via UI or `Link.open_serial("COM8")`.
      **Expect:** connects; `text_desyncs == 0` (the startup banner is drained at open).
      **Failure:** a stale reader still holding the port, or a banner change.

- [ ] **A2 — control plane round-trips.** `getstate`, `getpos`, `status cfg`.
      **Expect:** one line per command, no orphans.
      **Failure:** a multi-line reply is desyncing the text sink — the bug class
      `pingnode all` had.

- [ ] **A3 — status frame is complete.** `link.get_status()`
      **Expect:** 30-byte `STATUS_RSP` (0xA7) with plausible `state`, `buf_count`,
      `queued_us`, `pos[4]`, `expected_seq`.
      **Failure:** version skew — firmware and host disagree on the frame. Check
      `STATUS_RSP_SIZE` on both sides. (`web/demo/transport.js` is *known stale*
      here — still expects the retired 9-byte 0xA6.)

- [ ] **A4 — integrity / conformance.** `python -m host.diagnostics.test_comms --port COM8`
      **Expect:** PASS.
      **Failure:** framing, CRC or seq handling — before any motion is involved.

- [ ] **A5 — go-back-N recovers.** Stress stream (~2000 packets).
      **Expect:** resends occur and are **all deduped**; final `pos` exact.
      Reference run: 2000 packets @ 120 sps → 229 go-backs, 3638 resends, position exact.
      **Failure:** the seq duplicate guard — motion would silently duplicate.

- [ ] **A6 — ACK coalescing.** Saturating stream.
      **Expect:** ≈8.0 packets per ACK frame (`ACK_COALESCE_MAX`). The K counter
      is the primary trigger, not drain-empty.
      **Failure:** a flush is firing per packet — works, but throughput will be poor.

- [ ] **A7 — poll during stream.** Stream, and poll status throughout.
      **Expect:** status keeps updating mid-stream; `buf_count`/`pos` advance.
      **Failure:** the reader/writer split has regressed to seizure — the single
      claim the whole architecture rests on.

- [ ] **A8 — abort ramps.** Long stream, then `link.abort()`.
      **Expect:** ramp → ring flushed → IDLE, **position kept**; further packets
      NACK with `ABORTING` until the flag is consumed.
      **Failure:** abort wedges (the empty-ring barrier case) or lands ALARM.

- [ ] **A9 — pause/resume, estop, unalarm.**
      **Expect:** `pause` drains then PAUSED; `resume` continues; `stop` → ESTOP → ALARM;
      `unalarm` only from ALARM.
      **Failure:** the Core 1 state machine, independent of any bus.

---

## C — Full chain

Only once **S, A and B are each green**.

- [ ] **C1 — jog direction matches `step`.** Enable X. Note `getpos`. Jog X+ 10 mm
      from the UI. Compare the physical direction against **B9**.
      **Expect:** the axis moves the direction your machine calls +X.
      **This is the open question in the tree.** `axis.invert` was only just
      applied to jog (commit `d8bd12a`); X, Z and A are `invert=True` by default
      and no motor has ever verified it.
      **If jog and job agree with each other but both feel backwards → the
      config's `invert` is wrong, not the code.**
      **If jog and job disagree → the code regressed; that is exactly what `d8bd12a` fixed.**

- [ ] **C2 — jog distance is exact.** Jog 10 mm; measure physically.
      **Expect:** 10 mm, and `getpos` delta = `10 × steps_per_unit` (negated on
      an inverted axis — the Pico counts motor steps, not machine direction).
      **Failure with B8 ✓:** host-side units, not the machine.

- [ ] **C3 — blending.** Three rapid clicks in one direction.
      **Expect:** ONE continuous move of 3× distance, no stop between; UI shows
      `blend x3`; `buf_count` never reaches 0 mid-move.
      **Failure:** pacing — `LOW_WATER`/`LEAD_US` vs the ~100 ms poll period.
      A buffer shallower than the control loop *must* underflow.

- [ ] **C4 — reversal cancels.** Jog 50 mm, then click the opposite direction mid-move.
      **Expect:** ramps to rest, does **not** reverse, position intact, lands IDLE.
      **Failure:** the soft-abort path (§4.5).

- [ ] **C5 — no lost steps at speed.** Jog at max rate, cancel mid-move. `setorigin`
      first, and compare `getpos` against a physical mark.
      **Expect:** commanded and physical position still agree.
      **Failure:** the decel ramp is too aggressive for the pull-in rate —
      `DECEL_SPS2_*` (Z's value is an unverified placeholder).

- [ ] **C6 — two axes stay square.** Jog a 100 mm X move, then 100 mm Y; then a
      diagonal via a job.
      **Expect:** the diagonal is straight and 45°.
      **Failure:** Bresenham sync in Core 1, or mismatched `steps_per_unit`.

- [ ] **C7 — a real job.** Small SVG, pen/knife up (no media).
      **Expect:** completes, ends IDLE, position matches the plan's end point;
      progress and position update live throughout.

- [ ] **C8 — job geometry.** Same job on media. Measure a known dimension.
      **Expect:** within tolerance; closed shapes close.
      **Failure with C6 ✓:** planner/tool offsets, not motion.

- [ ] **C9 — pause/resume mid-job**, and **C10 — estop mid-job** (recover via
      `unalarm`, verify position is *not* trusted afterward).

---

## Known gaps — do not test these as if they work

Failures here are expected; they are unimplemented, not broken.

- **`rampStepInBounds()`** ([core1.cpp:94](../src/rp2350/core1/core1.cpp)) is a
  pass-through stub returning `true`. **No soft limits are enforced.** The
  `EMIT_SOFT_LIMIT` path around it is wired; only the predicate is missing.
  When implemented, limits must be converted mm→steps **and** through `invert`,
  because the Pico is entirely in motor frame.
- **`DECEL_SPS2_X/Y/Z/A`** are `#define`s, not config. **Z is a placeholder** —
  no `maxAccel` for it in the config.
- **`MSEG_FLAG_PATH_END`** is declarative only — excluded from `MSEG_FLAG_WIRE_MASK`,
  so the firmware does not act on it. Do not expect end-of-path behaviour.
- **TILE (0xAD) / TOOL (0xAC) magics** — defined in `shared.h`, packed by
  `host/protocol/packets.py`, handled by no `.cpp`. Unimplemented on both sides.
- **CFG_SET / CFG_GET** — handled in `data_plane.cpp`, parsed by the host reader,
  but the host never sends the request. Config over the wire does not work end to end.
- **`web/demo/transport.js`** still expects the retired 9-byte `0xA6` status
  frame. The web demo is broken against current firmware.
- **The firmware cannot distinguish a starved ring from a finished path.** A host
  that stalls mid-job stops the machine dead rather than ramping. No starvation
  timeout exists.

---

## Reporting

For each failure: **item ID**, what you observed, and the smallest domain it
reproduces in (S/A/B/C). The domain matters more than the symptom — it is
usually the whole diagnosis.

Useful while capturing:
- `JOG_DEBUG=1` — per-jog timing breakdown (click → reset_seq → session → drain)
  plus a per-packet trace, printed to stdout.
- The UI's session timer (under Last Command Status) is the same clock the jog
  status line stamps, so on-screen events and logs can be lined up directly.
