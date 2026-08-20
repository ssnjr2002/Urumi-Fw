# Homing

**Status:** §1 (the node: gate, pulser, `CMD_HOME`) implemented and building for
`db_node*`; the Pico supervisor (§2) and the host schema (§3) are still design.
Nothing has run on hardware yet — see §6.3.
**Cross-links:** [node_session_and_datum.md](node_session_and_datum.md) (`nodeOrigin`,
`NODE_FLAG_DATUM`), [coordinate_frames_and_limits.md](coordinate_frames_and_limits.md)
(the *home* frame), [wire_protocol.md](wire_protocol.md) (host↔Pico framing),
[engage_and_axis_map.md](engage_and_axis_map.md) (slots — homing deliberately does
not use them), [../web/src/machine/schema.ts](../web/src/machine/schema.ts) (config).

Limit switches are fitted to **X, Y, Z0, Z1**. Neither A axis has one. The A axes
will eventually take a rotary hall-effect index instead; §5 checks that this spec
can carry that without redesign, but nothing rotary is specified here.

---

## 0. The decision

**The node owns both the stop and the motion. The Pico relays and supervises. The
host owns the configuration and the sequence.**

The switch is wired to the node, so only the node can react to it within a step.
Once the node is reacting to the switch anyway, having it also *generate* the
motion costs almost nothing — a short timer ISR — and buys the property that
makes everything else simple: **the bus stays free during a home**, so the Pico
can poll progress live instead of interleaving polls into a step stream.

Three loops were considered:

| loop | verdict |
|---|---|
| host → Pico → node | worst. USB + bus latency in the stopping path. |
| Pico → node (streamed) | works, but the Pico must emit the seek in chunks and stop the stream to poll. All that machinery buys nothing the node-side gate does not already give. |
| node alone | chosen. |

### What homing motion is not

Homing is **uncoordinated single-axis motion**. It needs no Bresenham, no
lookahead, no jerk limiting, and no agreement with the host's velocity model.
`AxisConfig.maxAccel` and the planner's profile play no part. The node's ramp is
a linear interval decay (§1.3) and shares nothing with `emitMicroSegment`.

The one thing rate shaping *is* for is seek time. At the pull-in rate alone
(~1000 sps) a full-travel seek is 60–80 s per axis on this machine's
`stepsPerUnit` values — X is 160 steps/mm over 500 mm, and Z is 1200 steps/mm
over 50 mm, which is just as slow despite the short travel. The ramp exists to
recover that, nothing more.

---

## 1. Node

Everything in this section is compiled only where a switch is wired
(`#ifdef HAL_HAS_LIMIT_SWITCH`). Today that symbol exists on `avr128db32` only —
see §6.2.

### 1.1 The stream gate

**Implemented.** `src/node/types/stepper/stepper.cpp`, RX ISR stream path.

A node with a switch **refuses every stream step while its switch is asserted, in
both directions** — not only the direction that digs in deeper.

Direction-blindness is the point, not a simplification. During a job there is no
legitimate reason for an axis to be sitting on its hard limit, so there is nothing
to distinguish: whatever put it there, the correct response is to stop moving.
Allowing the escape direction would mean trusting a stream the Pico is emitting
open-loop, with no knowledge that the axis is pinned, to be the thing that rescues
it. The consequence is deliberate — **the stream can never drive off the switch.**
Recovery is a homing retract (§1.2), which is supervised and step-budgeted.

Three properties:

- **Always on.** Every machine state, no arm command, no homing mode. A limit trip
  during a job is blocked at the node, not merely reported.
- **Immediate.** The refusal is on the raw pin read, before the DIR write and
  before the pulse, so a real trip stops on the very next step. Nothing is
  debounced ahead of the refusal — waiting out a threshold before refusing would
  let the axis run thousands of steps further into the hard stop.
- **Position holds itself.** A refused step advances nothing, so
  `absolutePosition` *is* the trip position. No latched position field, no extra
  status bytes.

```c
const bool limAsserted = HAL_LIMIT_ASSERTED();
if (limAsserted) {
    if (++limitBytesAsserted - limitRunBase >= LIMIT_LATCH_BYTES)
        limitLatched = true;
} else {
    limitRunBase = limitBytesAsserted;
}
if (limAsserted || limitLatched) return;   // refuse the step
```

**Sticky vs momentary.** An intermittent switch that chatters would otherwise drop
steps silently and forever, with no way to tell that from a real trip. So the
refusal is instant but its *persistence* is earned: an assertion sustained past
`LIMIT_LATCH_MS` (500 ms) latches the gate shut, while a shorter run opens again
by itself on release. A glitch therefore costs a few steps and self-heals; a
genuine trip stays shut until a retract clears it (§1.2). Nothing in the stream
path can clear the latch, which is the whole reason it exists.

**The time base is stream bytes, not steps.** A stream byte is 11 bit-times (start
+ 9 data + stop) at `RS485_BAUD` — a fixed 11.9 µs tick available with no timer
read inside the ISR. Counting steps instead would measure *distance*: a slow axis
would take minutes to reach a threshold a fast one crossed in half a second.
`LIMIT_LATCH_BYTES` works out to ~41,890 and is computed at compile time from the
baud rate.

**`limitBytesAsserted` is monotonic and never reset.** The current run is
(total − base), with the base snapshotted on each release, so the run comparison
costs the lifetime figure nothing. That figure is the diagnostic worth having: a
switch that keeps chattering accumulates a large total while no single run ever
latches, which is exactly the fault that is otherwise invisible from the bus. It
is not yet readable over the bus — see §6.5.

`NODE_FLAG_LIMIT` (0x04) is published from `node_loop()` as *live pin OR latch*.
It is set from loop context, never the ISR, because `node_set_flag()`
read-modify-writes a byte the core's ENABLE/DISABLE/DATUM handlers also touch. The
flag is reserved on **every** node type and board, not only those with a switch:
the flags byte has one meaning across the bus, so the master decodes a status
reply without first knowing which board answered.

### 1.2 Seek and retract, decided from the pin

The node has one wire from the switch. It can see *pressed* or *not pressed*. It
cannot see which end of the axis it is at, nor what a move is for. Yet the two
homing moves need opposite treatment:

- **Seek** — starts off the switch, drives toward it, must stop when it presses.
- **Retract** — starts *on* the switch, drives away, must keep going for a fixed
  distance *while pressed*.

So "stop while pressed" cannot be the rule: a retract starts pressed and would
never move. Earlier drafts solved that by having the node hold an `approachDir` in
RAM, refreshed by each `CMD_HOME`, so it could tell "pressed and digging in" from
"pressed and escaping". That is gone.

**The rule is one pin sample, taken when `CMD_HOME` is accepted:**

| pin at command entry | mode | stop condition |
|---|---|---|
| clear | seek | switch asserts |
| asserted | retract | step budget exhausted; switch ignored throughout |

The mode is fixed once, at entry, and never re-evaluated. Inside a retract there
is nothing to detect. Inside a seek the level suffices, because a seek starts
clear by construction. So the node holds **no direction state at all** — no
`approachDir`, no boot sentinel, no previous-sample bit, and no seek/retract bit
in the payload.

It also removes the boot-degradation path the old §1.2 needed. A node that boots
with its axis already parked on the switch reads the pin as asserted and retracts
correctly on the **first** command, with no wasted move and no special case.
Nothing is lost by having no memory across a reboot, because a reboot clears
`NODE_FLAG_DATUM` anyway and the axis must be re-homed regardless.

**Coupling with the latch runs one way only.** The pulser never *reads*
`limitLatched` — deciding from the latch rather than the pin would reintroduce the
boot case, since latch-false does not imply pin-clear. It only ever *writes* it,
in exactly one situation: **clearing the latch after a retract that both ran its
budget and left the pin clear.**

Only a retract, because a seek by definition ends sitting on the switch — clearing
there would clear the latch at the one moment it should be set. And only a
*verified* retract: a move that used its whole budget and is still asserted did
not get off the switch (under-budgeted, wrong direction, or a stuck switch), and
leaving the latch set is the correct outcome. This is the only way the latch is
ever cleared short of a power cycle, which is why it has to exist — a homing move
that ran and finished is precisely the deliberate operator act meaning
"recovered".

The normal sequence ends on a retract-to-backoff, so a clean home leaves the node
unlatched and streamable with nothing special-cased.

### 1.3 The pulser

**Implemented.** `ISR(TCA0_OVF_vect)` in `stepper.cpp`.

A second timer generates the step train. **TCA0** in NORMAL mode — TCB0 stays the
step-pulse one-shot for both this path and the stream path, so a step is shaped
identically however it was requested. `PER` is the step interval and the ISR
rewrites it as the ramp decays. TCA0 is otherwise unused on a stepper node: it
backs `analogWrite()` PWM, which nothing here calls, and `millis()` lives on a TCB
under DxCore, so taking it does not disturb timekeeping.

Prescaler div8 gives 3 MHz on the 24 MHz DB32 (2.5 MHz on a 20 MHz ATtiny), which
keeps every useful rate inside a `uint16` interval so the ISR stays 16-bit. The
tick rate is derived from `F_CPU`, not written down as a constant — the slowest
expressible step is ~21.8 ms on the DB32, and `homingArm()` rejects anything
slower rather than truncating it.

**Both stop conditions are checked before the step, not after**, so a move never
takes one more step past the thing that ended it. On a seek that is physical: the
switch is the target and overshoot is travel into the hard stop.

**Stopping is split across contexts.** The ISR halts the timer and sets a
`homingFinished` flag; `node_loop()` does the rest. It has to be this way —
clearing the latch and publishing `NODE_FLAG_HOMING` both go through
`node_set_flag()`, which read-modify-writes a byte the core's ENABLE/DISABLE/DATUM
handlers also own, so it is loop-context only.

The ramp is a linear decay of the step interval toward a floor:

```
interval = start;
// each step:
if (interval > floor) interval -= rampStep;
```

`rampStep` is computed once when the command is accepted, from `ramp_steps`
(§1.4). Linear-in-interval is not constant acceleration — true constant accel
falls as ~1/√n — but it is gentler early, which is the direction that matters for
not stalling, and homing has no reason to match any other profile.

Other properties:

- Increments `absolutePosition` exactly as the stream path does. There is one
  counter on the node with one meaning (§4).
- **Does not require `ENGAGE`.** It runs while `slot == SLOT_NONE`. This is what
  makes dual-head Z homing trivial: each head's Z homes as its own transaction,
  with no axis-map juggling and no contention for the Z slot.
- Killed by `CMD_DISABLE`, which is already on the broadcast allowlist, so the
  existing estop sweep stops a home with no new mechanism.

**ISR budget.** Keep it lean: no floating point, no `delayMicroseconds`, no SPI.
This is also why the ramp is integer decay rather than the `sqrtf` Core 1 uses.

### 1.4 `CMD_HOME` (0x24)

**Implemented.** Constants in `include/common.h`, handler and `homingArm()` in
`stepper.cpp`.

11-byte payload; 15 bytes on the wire including framing, against `MAX_PACKET_LEN`
32.

| field | type | meaning |
|---|---|---|
| `dir` | u8 | wire dir bit — which way this move goes |
| `start_interval` | u16 | µs — pull-in rate |
| `floor_interval` | u16 | µs — cruise rate |
| `ramp_steps` | u16 | steps from start to floor; 0 = no ramp |
| `max_steps` | u32 | runaway budget |

**One direction field, and no mode field.** Earlier drafts carried both an
`approach_dir` (for the node to retain) and a `flags` bit selecting seek or
retract. Both are gone: the node retains nothing, and the mode comes from the pin
sample (§1.2). `dir` now means only *which way to move* — the host knows which way
that is for each leg of the sequence (§3.4), and the node does not need to know
what the move is for.

**Intervals are microseconds, not timer ticks.** The node converts on receipt.
This keeps board clock differences (ATtiny at 20 MHz, DB32 at 24 MHz) out of the
host and out of config — there is no node clock field in the schema, and there
does not need to be one.

`ramp_steps` rather than a per-step delta, because a delta needs sub-µs resolution
to be expressible (at 1 tick/step it is 0.4 µs). The node computes `rampStep =
(start_ticks − floor_ticks) / ramp_steps` once, at arm time.

**Arming rejects rather than clamps.** A zero interval, `floor > start`, a zero
budget, or an interval too slow for the 16-bit timer all NAK. Every one of them is
a host-side config or arithmetic error, and a clamped homing move would run at a
rate nobody asked for, into a hard stop, while reporting success. A NAK also tells
the master something a failed move cannot: that the move never started.

DIR is written once at arm time, in loop context, where it absorbs the 5 µs DM542
setup guard. The pulser ISR therefore never spins on it — the one long-ISR wart
§1.7 notes on the stream path does not reach this one.

### 1.5 Terminal states

Reported through the flags byte. `NODE_FLAG_HOMING` (0x08) is set while the pulser
runs. The master polls; the node never announces (§1.6).

| flags | after a seek | after a retract |
|---|---|---|
| `HOMING` set | still running | still running |
| `HOMING` clear, `LIMIT` set | **found** — the counter is the trip position | failed to get clear → fault |
| `HOMING` clear, `LIMIT` clear | budget exhausted, switch never reached → fault | **done** — latch cleared |

The same two bits mean opposite things for the two modes, which is not a defect:
the *master* knows which move it sent, even though the node does not. The node's
own rule stays "sample the pin, run, stop"; interpretation is the supervisor's job
(§2.3).

Plus, master-side: no reply within `RESPONSE_TIMEOUT_MS` → bus/node fault.

### 1.6 No NACK path

A node cannot signal asynchronously — it never transmits unless addressed, because
N nodes answering at once is a collision. Nothing here needs it. The "you may not
do that now" statements live where they can be acted on: host→Pico gets the
existing `MSEG_NACK_BAD_STATE` (§2.2), and Pico→node needs nothing, since the
master learns from the next poll.

### 1.7 Can the node talk while pulsing?

Yes, with margin.

- AVR ISRs do not nest — the RX ISR and the pulser ISR cannot preempt each other,
  so there is no re-entrancy hazard.
- The pulser ISR is ~3–5 µs at 20 MHz. At an 8000 sps cruise it fires every
  125 µs, occupying ~3% of the time.
- A 9-bit frame at 921600 baud is ~11.9 µs, and the USART has a 2-byte RX FIFO →
  ~24 µs of slack before overrun. A 3–5 µs block is nowhere near it.

What makes this comfortable rather than marginal: **command handling and TX are
not in an ISR at all.** `dispatchCommand` runs from `loop()`, so parsing,
`sendCommandPacket`, and the DE-toggle delays in `HAL_RS485_TX_BEGIN/END` are
fully preemptible by the pulser. Answering a poll costs the step train a few µs
of jitter, not a pause.

One existing wart: the RX ISR's `delayMicroseconds(5)` DIR-setup guard spins 5 µs
inside an ISR. It cannot bite here — homing does not stream, so that path is not
taken — but it is the one long ISR on the node.

---

## 2. Pico

### 2.1 What the Pico does not do

- **Generates no homing motion.** It relays `CMD_HOME`, polls, and reports.
- **Parses no config.** `config_store` owns *"one opaque msgpack blob"* and reads
  no fields; every parameter arrives from the host as a plain number.

Note that finishing the deferred Phase 2 MCFG work would not change this. Its
mechanism is a CRC32 equality check — an *agreement* mechanism, not an *access*
one. It proves both sides hold the same bytes; it never lets the Pico read a
field. Homing is not blocked behind it.

### 2.2 The `home` command

Control plane (text, one line in, one line out) — homing is infrequent,
parameterised, and wants a reply, which is that plane's exact profile. The data
plane is for high-rate windowed streams and would need new binary framing for no
gain.

```
home <axis> <seek|retract> <approach_dir> <start_us> <floor_us> <ramp_steps> <max_steps>
```

`seek|retract` follows the existing `on`/`off` token style (`parseState`).
`<axis>` reuses `axisMask()` but **rejects any mask with more than one bit** —
one axis at a time (§3.5).

Replies `ok`, or `err bad_state` / `err arg` / `err node N timeout`.

**It must not block.** A home takes ~13 s, and the control-plane contract is one
reply line per command. A blocking `home` would freeze the plane for the whole
seek — no `getstate`, no `stop`, **no abort** — on a command that is driving an
axis at a hard stop. So it returns immediately and the machine enters
`STATE_HOMING` (already reserved in `shared.h`), exactly as a job does:

- success: `STATE_HOMING` → `STATE_IDLE`
- fault: `STATE_HOMING` → `STATE_ALARM`, `alarmReason = ALARM_HOME_FAIL`
- abort: the existing `stop` works unchanged

`STATE_HOMING` joins the data-plane allowed-state matrix, so a job stream
arriving mid-home gets the existing `MSEG_NACK_BAD_STATE`.

### 2.3 The supervisor

Core 0 polls `CMD_NODE_STATUS` at **20–50 ms** while in `STATE_HOMING`. This
lives in the `core0.cpp` loop, *not* as a blocking wait inside the command
handler — that is what keeps `stop` responsive.

Poll cadence does not affect accuracy. Because the gate prevents any motion past
the trip point, the position is exact whenever it is read; the cadence decides
only when the Pico *notices*.

The timeout is derived from the payload just sent, not guessed:

```
ramp_steps × avg_interval  +  (max_steps − ramp_steps) × floor_interval
```

× 1.2. For X that is ~13.5 s — tight enough to detect a fault, unlike the naive
`max_steps × start_interval` bound, which gives a useless 88 s.

### 2.4 Core 0 → Core 1

The FIFO word is `[23:16] payload | [15:8] cmd | [7:0] node` — **one payload
byte**, so the 12-byte `CMD_HOME` payload does not fit. Use the existing
multi-word precedent: `FIFO_STEP_DEBUG` pushes a tagged word then a continuation.
`FIFO_HOME` follows the same shape — tag word plus 3 continuation words.

### 2.5 `setorigin` needs one new argument

Today `setorigin [axes]` hardcodes the datum to zero:

```c
nodeOrigin[n] = nsPos(st);  machinePos[i] = 0;
```

That is exactly the switch-at-origin case, and it already works. A far-end switch
needs `machinePos[i] = hardTravel × stepsPerUnit`, which cannot be expressed.
Extend it:

```
setorigin [axes] [pos_steps]        # pos_steps defaults to 0
```

`machinePos` is in the **wire** frame (steps), so the host converts. Keeping the
datum here rather than folding it into `home` has two benefits: `home` stays
purely about motion, so the retract and re-approach passes carry no datum
baggage; and this inherits `setorigin`'s existing estop-window handling (the
`alarmAtEntry` snapshot), which is subtle code that should not exist twice.

---

## 3. Host

### 3.1 Schema

`AxisConfig` gains one optional field. The group is optional; the fields inside it
are not — one decision per axis ("does this have a switch"), and if yes,
everything must be said.

```ts
export interface LinearHoming {
    readonly kind: "linear";
    /** Physical distance between hard stops (mm). The budget derives from this. */
    readonly hardTravel: number;
    /** True = switch at the origin end, false = at the far end. */
    readonly atOrigin: boolean;
    readonly seekFeed: number;    // mm/s, fast pass
    readonly latchFeed: number;   // mm/s, slow re-approach
    readonly backoffMm: number;   // must exceed the switch's RELEASE hysteresis
}

export type HomingConfig = LinearHoming;   // | RotaryHoming later — see §5

export interface AxisConfig {
    // …
    readonly homing?: HomingConfig;
}
```

**Absent means no sensor**, which handles both A axes by omission rather than by a
`homePresent: false` flag.

**Nothing goes in `DEFAULTS.axis`.** Per that file's own rule, calibration must
come from config with no silent fallback, and every field here is calibration: a
wrong `atOrigin` drives the gantry into a hard stop, and absolute feed defaults
cannot span X (`maxFeed: 90`) and Z (`maxFeed: 12`).

**Why two fields and not one `tripPos`.** A limit switch is always at one end or
the other, so the trip position is binary. `tripPos: 700` on a 1200 mm axis is
nonsense that a validate rule would have to reject; a bool cannot be 700. Note
this argument inverts for rotary — see §5.3.

**The `kind` tag ships now, with one member.** TypeScript cannot narrow `homing`
from the sibling `rotary` flag, so the discriminant must be inside the union.
Tagging from day one makes adding `RotaryHoming` a pure addition rather than a
breaking change to every config that already has homing, and keeps
`json/load.ts` from having to sniff structurally. Precedent: `Anchor` in
`frames.ts`.

Everything else derives:

| derived | expression |
|---|---|
| trip position (mm) | `atOrigin ? 0 : hardTravel` |
| approach direction (mm) | `atOrigin ? −1 : +1` |
| runaway budget | `hardTravel × stepsPerUnit × 1.1` |
| `setorigin` argument | `tripPos × stepsPerUnit` |

### 3.2 `invert` belongs here, and only here

Config states where the switch physically is; the wire dir bit is an electrical
fact. `invert` is already the mapping between them for the streaming path, so the
direction of each homing leg is derived from it:

```ts
const approachPositive = !homing.atOrigin;                       // mm frame
const approachDir = approachPositive !== axis.invert ? 1 : 0;    // wire bit
```

`approachDir` is a **host-side local**, not node state: it is what the host uses
to fill `CMD_HOME.dir` — as-is for a seek leg, inverted for a retract leg. The
node stores no direction of its own (§1.2).

Putting a raw approach dir in config instead would create two independent
direction facts that can disagree — an axis that streams one way and homes the
other, with nothing to catch it. The node never hears the word `invert`.

### 3.3 Validation

New rules appended to `RULES`, hanging off the existing `namedAxes()`:

| rule | level |
|---|---|
| `rotary: true` with `kind: "linear"` (and the converse) | error |
| budget has a source: `hardTravel > 0` | error |
| `maxTravel <= hardTravel` when both set (`maxTravel: 0` = uncapped, exempt) | error |
| `seekFeed`, `latchFeed` > 0 and `<= maxFeed` | error |
| `latchFeed < seekFeed` | warning |
| X and Y both have `homing`, or neither | warning |

The feed-ceiling rule is an **error**, unlike the house convention for targets.
`overCeiling` warns because targets get clamped; a seek feed above the ceiling is
clamped by nothing on the node — it stalls or overruns the switch.

`maxTravel` is untouched by this work. It remains the soft-limit envelope;
`hardTravel` is the physical extent between hard stops. They are different
numbers and both are needed.

### 3.4 The sequence

Three moves, then the datum. All host-side; the Pico gains no sequencer and the
node gains nothing at all — it is the same command three times with different
parameters.

Worked example — X, switch at the far end, 500 mm, 160 steps/mm:

```
home x seek    1 1000  125   2000 88000   → ok    # fast: 1000µs→125µs over 2000 steps
                                                  (poll getstate until not HOMING)
home x retract 1 1000  1000  0    320     → ok    # 2 mm back-off, constant rate
                                                  (poll)
home x seek    1 20000 20000 0    8000    → ok    # slow re-approach, no ramp
                                                  (poll)
setorigin x 80000                          → ok    # 500 mm × 160 steps/mm
```

`ramp_steps: 0` with `start == floor` is the natural spelling for "constant
rate"; it needs no special case on the node, since the ramp loop simply never has
anything to subtract.

**Why two passes.** The electronic error is zero on both — the gate guarantees
that. What the slow pass removes is the *speed-dependent mechanical* error: at
50 mm/s the switch actuates later and stopping distance varies with load; at
3 mm/s neither does. It matters most on Z, where 1200 steps/mm makes one step
0.83 µm — well below what a mechanical switch repeats to. And it costs nothing,
being pure host sequencing.

**The back-off must clear the switch's release point, not its trip point.** There
is hysteresis between them, typically 0.5–2 mm. Back off too little and the
re-approach begins already triggered.

### 3.5 One axis at a time

The operator picks an axis; there is no ordering policy and no batch sequencer.
Batched homing later is pure host sequencing over the same `CMD_HOME`, so neither
the Pico nor the node changes when it arrives.

Parallel homing was considered and dropped. The one case that would have forced
it — squaring a dual-motor gantry — does not exist here: **the two Z axes belong
to decoupled tool heads, not to one gantry**, so there is no squaring to
preserve.

The interlock that matters is already covered by `STATE_HOMING` in the
allowed-state matrix: no job stream during a home, and no second home.

**Consequence for the UI:** with ordering gone, nothing enforces Z-first. Homing X
or Y with a tool down will drag it across the material. That warning now lives in
the operator flow rather than in a sequencer, and it needs a home.

---

## 4. What does not change

The datum machinery, the axis map and `ENGAGE`, the stream byte format, and the
MSEG path are all untouched. Homing bolts on beside them.

In particular, homing establishes `nodeOrigin` through the *existing* mechanism.
`absolutePosition` is not a position — it is a free-running tally with an
arbitrary origin. `nodeOrigin` is the record of what that tally read when the axis
was somewhere nameable:

```
home_mm = (absolutePosition − nodeOrigin) / stepsPerUnit
```

At the trip the tally reads `P_trip` and the axis is physically at `tripPos`, so

```
nodeOrigin = P_trip − tripPos × stepsPerUnit
```

With `atOrigin: true` this collapses to `nodeOrigin = P_trip` — the count at the
switch *is* the count at origin, and there is no arithmetic at all. That the
common case needs no correction is a decent sign the field choice is right.

This is also why the node's counter keeps running during a home: there is one
counter with one meaning regardless of how the axis was moved. Having the node
zero itself at the trip would give `absolutePosition` two meanings depending on
how it was last set, and would save nothing — the `tripPos` term would simply move
elsewhere.

---

## 5. Does this spec carry a rotary index?

Not built, and no rotary implementation is specified here. This section exists to
check that the spec will not have to be redesigned when the A axes get a
hall-effect index, and to name what must not be foreclosed.

### 5.1 What transfers unchanged

The `CMD_HOME` payload, the pulser and its ramp, the non-blocking `home` command,
`STATE_HOMING`, the poll loop, the timeout derivation, `setorigin <axes>
<pos_steps>`, the `nodeOrigin` arithmetic, and the fault handling. All of it.

The budget becomes ~1.1 revolutions (≈3520 steps at 8.890 steps/deg) rather than
`hardTravel × stepsPerUnit` — a different number in the same field, not a
different mechanism.

### 5.2 What genuinely differs

An index is not a barrier. There is no hard stop, the axis must keep rotating
during normal cutting, and blocking motion at the index would freeze A every
revolution. So:

- The **gate must be absent**, not merely unused, on a rotary node.
- Nothing **self-terminates** — the node must decide to stop.
- The **stopping position is irrelevant**; what matters is where the sensor
  asserted.

The mechanisms converge more than that suggests. If the pulser samples the pin at
the top of each step ISR and declines to step when asserted, then for a barrier
the counter cannot advance and *is* the trip position, and for an index the pulser
halts at the edge and the counter *is* the index position. Both are "check the pin
in the step path; if asserted, do not step." The differences reduce to **scope**
(always vs. homing-only) and **directionality** (barrier is directional, index is
not).

Usefully, that means **no separate latch field and no change to the status
payload** — the existing position tail carries the right answer either way,
because the axis never moves past the detection point.

The retract pass survives, for a different reason: a magnet asserts over an arc
several degrees wide, so the datum is its *leading edge*, and a home that starts
with A already inside the arc would detect the index immediately, at the wrong
angle. A move to escape the arc must precede the seek. The slow re-approach, by
contrast, can be dropped — accuracy is one step (≈0.11°), set by per-step
sampling rather than by speed.

### 5.3 Config shape

Different, and legitimately so. The §3.1 argument against a free `tripPos` inverts
here: an index mark can be at any angle, so a free number is exactly right, and
`hardTravel` / `atOrigin` / `backoffMm` / `latchFeed` are all meaningless. Sketch
only:

```ts
export interface RotaryHoming {
    readonly kind: "rotary";
    readonly indexAngle: number;   // deg the axis reads at the index mark
    readonly escapeDeg: number;    // clear the magnet arc before seeking
    readonly seekFeed: number;
    readonly maxSteps: number;     // ~1.1 rev; not derivable from maxTravel (0)
}
```

Adding it later touches the host alone, because the `kind` tag is already there
(§3.1).

### 5.4 What must not be foreclosed

Only two things, both free:

1. **Keep the gate behind `#ifdef HAL_HAS_LIMIT_SWITCH`, and keep "stop the
   pulser" separate from "refuse a stream step."** They are naturally separate —
   one lives in the RX ISR, one in the pulser. If they collapse into a single
   always-on directional check, a rotary node inherits behaviour it must not have.
2. **Do not let `CMD_HOME`'s completion semantics assume "stopped because
   blocked."** The three terminal flag states (§1.5) already work for both kinds;
   just do not add a barrier-specific shortcut.

---

## 6. Open

### 6.1 Hard limits are not observed during a job

`emitMicroSegment` is a tight step loop with no bus transactions, and nothing
polls between segments either. The **node** still blocks motion into the switch —
that part is safe — but the Pico does not learn about it until the job ends, and
keeps streaming into a frozen axis meanwhile, cutting at a position that is wrong
and diverging.

Closing this means interleaving a poll at segment boundaries, which costs a stream
gap and is genuinely new machinery. Out of scope here, but it is a real gap rather
than an oversight.

### 6.2 ATtiny3224 has no limit pin

`HAL_LIMIT_SWITCH_PIN` is defined on `avr128db32` only (PD1, active-low against
the internal pull-up, so a severed wire reads as asserted). Which board carries
each of X / Y / Z0 / Z1 needs confirming; if any is an ATtiny3224, that board
needs a pin chosen and the four `HAL_LIMIT_*` symbols defined — the whole gate is
`#ifdef HAL_HAS_LIMIT_SWITCH`, so an ATtiny build today compiles it out entirely.

### 6.3 Numbers to measure before first run

- **Switch over-travel.** This bounds `seekFeed` — not `maxFeed`. The fast pass
  must stop within the travel between trip point and hard bottom-out, or it trips
  the switch and then crashes into it.
- **Switch release hysteresis**, which sets the floor for `backoffMm`.
- **Pull-in rate** per axis, which sets `start_interval` without stalling.

### 6.4 Deferred

- Batched / multi-axis homing (§3.5) — host-only when wanted.
- Where the "Z first" operator warning lives (§3.5).
- Rotary index for the A axes (§5).

### 6.5 `limitBytesAsserted` is not readable over the bus

The lifetime chatter count (§1.1) is maintained but not exposed. Exposing it means
extending the stepper status tail, which changes a payload the Pico parses — worth
doing as its own step rather than bundled into the gate.

---

## 7. Bench-confirmed reference

Values found by running the raw console `home <node> <dir> <start_us> <floor_us>
<ramp_steps> <max_steps>` command (§2.2's bare-minimum bench version) directly
against hardware, kept here so a future session does not have to re-derive them
from `comms.json` and re-discover the same corrections. These are per-node
findings, not spec — §3.4's derivation is still how a new axis gets a starting
point.

**Per-node direction and polarity, as confirmed on the bench:**

| node | axis | seek `dir` | `LIMIT_ACTIVE_HIGH` |
|---|---|---|---|
| 1 | X | 1 | yes — switch reads inverted from the active-low default |
| 2 | Y | — (not recorded; only polarity was flagged) | yes |
| 3 | Z0 | 0 | no — default active-low is correct, unconfirmed against a mismatch report |
| 6 | Z1 | not yet probed | not yet probed |

**Node 3 (Z0) four-leg sequence**, confirmed working on hardware:

```
home 3 0 667  83   118 180000    # fast seek
home 3 1 1000 1000 0   5000      # 2mm-nominal retract — needed ~5000 steps, not 2400
home 3 0 3000 3000 0   6000      # slow latch seek — 8000/8000 was too slow (~40s); 3000/3000 ≈12s
home 3 1 1000 1000 0   5000      # final backoff
```

The corrections worth remembering for the next axis:

- **A budget derived from `hardTravel`/`stepsPerUnit` alone tends to run short.**
  Z0's retract needed roughly double the naive 2 mm figure (5000 steps ≈ 4.2 mm at
  1200 steps/mm) to reliably clear switch hysteresis — budget with margin, then
  trim down only if bench testing shows it's excessive.
  Following that ~2× margin is not automatic, though: a *bigger* retract in turn
  means leg 3's own budget has to grow to match (see below), not stay at the
  original 2mm-derived figure.
- **Leg 3's budget must exceed leg 2's actual retract distance**, not the
  originally-planned one. It has to re-cross whatever leg 2 actually travelled to
  find the switch again; if leg 2 grows, leg 3's budget has to grow with it or it
  reports a false switch-never-found fault.
- **A slow latch-seek interval derived by naive proportion can be too slow to be
  practical**, even though it's not wrong in principle. `8000/8000` µs (125 sps,
  0.10 mm/s at 1200 spu) took ~40 s to cross the retract distance on Z0; `3000/3000`
  µs (333 sps, 0.28 mm/s) cut that to ~12 s while staying well below seek speed.
- **This firmware's structure — seek / fixed-distance retract / slow re-seek /
  fixed-distance retract — matches the standard approach used by GRBL, Marlin, and
  FluidNC.** None of those stop the retract on switch-release either; the release
  edge is the noisiest point on the switch's travel (bounce, compliance, thermal
  drift), so all of them retract a fixed, tuned distance rather than watch the pin
  during that leg. Klipper is the one exception that supports pin-gated retract as
  an option, but even its common recipe defaults to distance-based. No change is
  planned here on this basis — noted so it isn't re-litigated per axis.

Z1 (node 6) has not yet been probed on the bench; treat the §3.4-derived starting
values as unverified until it is.
