# Homing

**Status:** §1 (the node) and §2 (the Pico: `lin_leg`/`rot_leg`, `setorigin <pos_steps>`, the
supervisor in `src/rp2350/core0/homing.cpp`) implemented and confirmed on
hardware — **except** the `<intent>` argument and `NAK_INTENT_MISMATCH` (§1.2,
§1.4, §2.2), which build clean on both `db_node1` and `pico` but have not yet
run on real bus hardware, only against the Sim. §2.6's `ALARM_LIMIT_LATCHED` and
§3 (schema, validation, the host sequencer in `web/src/homing/`, and the demo
panel) are likewise implemented and exercised end to end against the Sim —
**not yet against hardware**, because the numbers in §6.3 are still unmeasured.
`atOrigin` in particular is a guess in every config that has it, and a wrong one
drives the axis into a hard stop at seek speed.
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
RAM, refreshed by each `CMD_HOME_LEG`, so it could tell "pressed and digging in" from
"pressed and escaping". That is gone.

**The rule is one pin sample, taken when `CMD_HOME_LEG` is accepted:**

| pin at command entry | mode | stop condition |
|---|---|---|
| clear | seek | switch asserts |
| asserted | retract | step budget exhausted; switch ignored throughout |

The mode is fixed once, at entry, and never re-evaluated. Inside a retract there
is nothing to detect. Inside a seek the level suffices, because a seek starts
clear by construction. So the node holds **no direction state at all** — no
`approachDir`, no boot sentinel, no previous-sample bit.

**A payload bit was added later, and it does not change any of the above.** See
"The intent bit" below — it rides alongside this decision, not inside it.

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

#### The intent bit checks the decision; it does not make it

**Implemented.** `CMD_HOME_LEG` payload byte 0 gained a second bit: `dir` in bit 0,
unchanged, and `intent` in bit 1. This does **not** reopen the question the rest
of this section just closed — the pin sample above is still the only thing that
decides seek vs retract, still taken once at entry, still never re-evaluated.
`intent` is a second, independently-arrived-at opinion that the host attaches so
the node can catch the two opinions disagreeing, rather than silently acting on
its own.

**The gap this closes.** Nothing before this checked that the host's plan and
the node's physical reality agreed. Concretely: the host derives a seek's budget
as a runaway cap — generous, because the switch is supposed to cut the move
short. If the host *thinks* it is arming a seek but the pin is already asserted
— stale state, a bounced or mis-wired switch, a prior leg that did not clear it
as expected — the node silently arms a **retract** instead. A retract ignores
the switch and runs its budget to completion. Same huge number, opposite
semantics: the axis travels the full seek-sized distance with nothing left to
stop it. That is the crash the two-pass, alternating-direction design in §3.4
exists to prevent, arrived at by a different door.

**The check, in the node's `CMD_HOME_LEG` handler:**

```c
const bool retract         = HAL_LIMIT_ASSERTED();   // unchanged: THE decision
const bool intendedRetract = (p[0] & 0x02) != 0;      // the host's prediction

if (retract != intendedRetract) {
    node_reply_nak(CMD_HOME_LEG, NAK_INTENT_MISMATCH, reply, replyLen);
    return true;
}
```

Stateless, same as the pin read itself: nothing is stored past the single
command, and a node with older firmware that never reads bit 1 behaves exactly
as before (the bit sits unread in a byte it already receives).

**A reasoned NAK, not the pre-existing generic one.** Every earlier `CMD_HOME_LEG`
rejection — bad interval, zero budget, no switch wired — answers with a bare
`return false`, which the core's dispatcher turns into `NAK_UNSUPPORTED`
(`common.h`) regardless of which of those it was. That was always slightly
wrong, and reusing it here would have been more so: `NAK_UNSUPPORTED` reads as
"this node does not do `CMD_HOME_LEG`", which is false — it does, just not under
this command's premise. `NAK_INTENT_MISMATCH` (`0x04`) is a new reason,
propagated through `RpcResult`'s existing `nakReason` field to
`rpcResultText()` and printed by `homingBegin()` as `nak intent_mismatch` — so
an operator sees a name that says what to check (re-read the switch) rather
than one that suggests a wiring or framing bug.

**Where `intent` comes from on the host: `HomingLeg.kind`, not a fresh guess.**
The plan already knows, leg by leg, whether it expects to start on the switch —
that is exactly what alternates through the four legs in §3.4. `BACKOFF` and
`PARK` start on it (they exist to retract off it); `SEEK` and `LATCH` start
clear. `homing/sequence.ts` derives `intendedRetract` from `leg.kind` and passes
it to `home()`, which packs it into bit 1 alongside `dir`. The Pico is a pure
relay for it end to end — `cmdHome()` parses a sixth text argument and hands it
straight to `homingBegin()` → `rpcHome()`, none of which inspect it.

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

### 1.4 `CMD_HOME_LEG` (0x24)

**Implemented.** Constants in `include/common.h`, handler and `homingArm()` in
`stepper.cpp`.

11-byte payload; 15 bytes on the wire including framing, against `MAX_PACKET_LEN`
32.

| field | type | meaning |
|---|---|---|
| `dir`/`intent` | u8 | bit0 = wire dir bit; bit1 = intent (§1.2's "The intent bit") |
| `start_interval` | u16 | µs — pull-in rate |
| `floor_interval` | u16 | µs — cruise rate |
| `ramp_steps` | u16 | steps from start to floor; 0 = no ramp |
| `max_steps` | u32 | runaway budget |

**One direction field, one CHECK field, and still no mode field.** Earlier
drafts carried both an `approach_dir` (for the node to retain) and a `flags` bit
*selecting* seek or retract. Both are gone, and neither is what `intent` is: the
node retains nothing, and the mode still comes from the pin sample (§1.2) alone.
`dir` means only *which way to move* — the host knows which way that is for each
leg of the sequence (§3.4). `intent` means only *what the host expects the pin
to say*, checked against it, never substituted for it.

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

**And the stream path must not write DIR while the pulser owns the axis.** Writing
it once is only safe if nothing else writes it afterwards, and something did:
`busQuiesce()` prefaces EVERY command frame with a NOP stream byte, a zero byte
has the slot's dir bit clear, and the stream handler reads that as "direction 0"
and drives DIR low. Since the supervisor (§2.3) polls the homing node every
`HOMING_POLL_MS`, the first poll after the arm yanked DIR out from under the
pulser and every step after it ran the wrong way. `absolutePosition` did not
notice — the pulser derives it from `homing.dir`, so the counter kept reporting
the direction that was *asked for* while the shaft went the other way, and only
the counter is visible over the bus. Both directions therefore looked identical.

The RX stream path now returns immediately while `homingActive`. It returns
before the limit accumulator too: during a home the pulser's own pin read is the
authority on the switch, and letting NOP bytes advance `limitBytesAsserted` would
move the baseline `homingFinish()` judges a retract against.

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

- **Generates no homing motion.** It relays `CMD_HOME_LEG`, polls, and reports.
- **Parses no config.** `config_store` owns *"one opaque msgpack blob"* and reads
  no fields; every parameter arrives from the host as a plain number.

Note that finishing the deferred Phase 2 MCFG work would not change this. Its
mechanism is a CRC32 equality check — an *agreement* mechanism, not an *access*
one. It proves both sides hold the same bytes; it never lets the Pico read a
field. Homing is not blocked behind it.

### 2.2 The `lin_leg` and `rot_leg` commands

Control plane (text, one line in, one line out) — homing is infrequent,
parameterised, and wants a reply, which is that plane's exact profile. The data
plane is for high-rate windowed streams and would need new binary framing for no
gain.

```
lin_leg <node> <dir> <start_us> <floor_us> <ramp_steps> <max_steps> <intent>
rot_leg <node> <dir> <start_us> <floor_us> <ramp_steps> <max_steps>
```

**ONE LEG, NOT A HOME**, and the verbs say so. These replace the single `home`
verb, which named the wrong thing in two different ways: for a linear axis it
was one leg of four, and for a rotary one it looked like the entire job. The
firmware runs legs. Sequencing them into a home, deciding when a pair is
complete, and turning the result into a datum are all the host's (§3).

**Addressed by BUS ID, not by axis.** Everything a leg produces is node-framed
— the span, the index in the node's own step counter, the limit latch (a switch
is wired to a node, not to a stream slot) — and nothing it produces is
slot-framed. `core0/position.h` states the rule these obey: *the NODE frame is
the truth, the SLOT frame is a view, and a view is never written directly by a
command handler.* Routing a leg through the axis map made a command that writes
only truths ask a view for permission first.

Two consequences, both wanted:

- **No `err unconfigured`.** An axis cannot be resolved without a committed
  `axis_map`, but a bus id needs no map at all, so a leg now runs during
  commissioning — which is precisely when homing matters. Homing an unbound
  head used to need a throwaway `axis_map - - - 4` to borrow a slot first.
- **The enable gate reads `nodeEnabled`,** not `axes_enabled`. The latter is
  only the former projected through the map (`core0/position.cpp`), so on a
  bound node the two agree and on an unbound one only the node mask exists.

The datum survives either way. `originInvalidate()` is node-framed and clears a
slot's homed bit only if some slot happens to point at that node;
`slotAdoptStatus()` recomputes `machinePos` from `nodeOrigin` on every later
bind. A leg run before the map and a map committed after it land correctly.

**Two verbs, so no argument means two things.** `rot_leg` has no `<intent>`,
because a rotary node has no limit pin: there is nothing for the host to predict
and nothing for the node to disagree with. Under the single overloaded verb that
argument was inert on half the nodes it could be sent to.

**The Pico probes the node's kind before arming.** `HOMING_KIND_*` is declared in
every stepper's status tail, so `homingBegin()` spends one extra transaction
(~1 ms) asking what the node is and answers
`err kind_mismatch node <n> is <k> want <k>` on a mismatch. The kind also arrives
in the arm ack — which is where the seek/retract classification reads it — but
that ack is sampled *after* the pulser has started, so checking only there would
let a `lin_leg` aimed at a rotary node run a full sweep before anyone noticed,
and a `rot_leg` aimed at a linear one drive into a hard stop hunting a dip that
does not exist. A node with no terminator at all (`HOMING_KIND_NONE`) fails the
same check, and more usefully than the bare NAK it would otherwise get: the error
names what the node *is*, not merely that it said no.

**No `<seek|retract>` token** on the linear side, dropped by design and not left
unbuilt. The node still picks the mode from one read of its own limit pin at arm
time (§1.2), which reproduces §3.4's sequence on its own: after a seek the switch
is asserted, so the next leg retracts; after the back-off it is clear, so the
next one seeks. What the master needs — WHICH mode ran, since the terminal flags
read oppositely for the two (§1.5) — comes back in the arm ack, whose LIMIT bit
*is* that pin read.

**`<intent>` is not that dropped token come back.** It carries no authority over
the mode. What it does is give the node something to check the pin read AGAINST:
`0` or `1`, the host's own prediction of whether this leg starts on the switch,
taken straight from which leg of the plan this is (§1.2's "The intent bit").
Agree and the leg arms as before. Disagree and the node NAKs
(`nak intent_mismatch`) instead of arming — the case this catches is the host's
plan and physical reality having quietly diverged, which used to run silently
under whichever leg's semantics the pin happened to pick.

One leg at a time, machine-wide: the supervisor holds a single claim, so a
second leg while one is in flight gets `err busy` (§3.5). Node addressing makes
concurrent per-node legs *expressible* and they are deliberately not built —
that is a separate decision about whether two axes may home at once.

Replies `ok`, or `err busy` / `err bad_state` / `err not_enabled` /
`err kind_mismatch ...` / `err usage` / `err range` / `err bad_reply` /
`err node N <reason>`, where `<reason>` includes `nak intent_mismatch` alongside
the pre-existing `nak unsupported` / `nak bad_token` / `nak bad_arg`.

**It must not block.** A home takes ~13 s, and the control-plane contract is one
reply line per command. A blocking leg would freeze the plane for the whole
seek — no `getstate`, no `stop`, **no abort** — on a command that is driving an
axis at a hard stop. So it returns immediately and the machine enters
`STATE_HOMING` (already reserved in `shared.h`), exactly as a job does:

- success: `STATE_HOMING` → `STATE_IDLE`, **or** → `STATE_ALARM` /
  `ALARM_LIMIT_LATCHED` if the axis ended parked on the switch (§2.6)
- fault: `STATE_HOMING` → `STATE_ALARM`, `alarmReason = ALARM_HOMING_FAIL`
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

**Superseded.** This section described packing the payload into single FIFO
words, which the IPC refactor removed: `RpcRequest` now carries a generic
`args[]` buffer sized by `RPC_ARG_MAX`, which is *defined as*
`CMD_HOME_LEG_PAYLOAD_LEN` (11) precisely because `CMD_HOME_LEG` is the largest payload.
So no continuation words, no `FIFO_HOME` tag, and no per-command packing: the
11 bytes are laid out once in `rpcHome()` and copied verbatim by `buildPayload()`
(`core1/rpc_server.cpp`).

The payload is **11 bytes**, not the 12 this section assumed.

### 2.5 `setorigin` needs one new argument

**Implemented.** `setorigin [axes] [pos_steps]`, with `pos_steps` defaulting to 0
and a mask that names only unbound axes answering `err unbound` rather than a
misleading `ok`.

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

### 2.6 IDLE between legs is a lie

**Implemented.** `ALARM_LIMIT_LATCHED = 6` in `shared_state.h`, the mask and
`resumeOrHold()` as described below — with one correction, at the end of this
section, to where the mask lives.

The supervisor sends `STATE_HOMING` → `STATE_IDLE` on every success, so between
leg 1 and leg 2 the machine reports IDLE while the axis is sitting on a latched
switch. It is not idle. It cannot move:

- the node's stream path refuses every step while `limAsserted || limitLatched`
  (§1.1), and refuses it **silently** — no NAK, no flag change;
- `data_plane.cpp` admits a job in IDLE with no check of any kind;
- Core 1 adds the emitted steps to `machinePos` regardless.

So a job started between legs streams normally, one axis does not move,
`machinePos` says it did, and `axes_homed` still reads set. The counters diverge
with nothing indicating it. IDLE is the symptom; the silent divergence is the
fault.

**The condition becomes an alarm.** A seek that ends on the switch enters
`STATE_ALARM` with `ALARM_LIMIT_LATCHED`. This costs nothing to gate: ALARM
already blocks the data plane, and `busGateDenies()` already admits ALARM, so
`home` and `setorigin` keep working and the four-leg sequence runs unchanged. No
new NACK and no new gate.

It is also safe against the one thing that would have killed it:
`reconcileValidity()` invalidates only on `STATE_ESTOP` / `ALARM_ESTOP` /
`ALARM_SOFT_LIMIT`, so a new reason does not destroy the datum or drop
`axes_enabled`. Entering ALARM on a *successful* seek therefore costs nothing —
and it is not "success produced an alarm", it is "the axis is now parked against
a limit", which is a condition, not an outcome.

**A latch mask, one bit per axis, is the durable truth** — per *node*, with the
per-slot `homingLatched` derived from it; see the correction below. `alarmReason`
is a single slot and can only name one thing: latch Z0, then fail a Y home, and
`ALARM_HOMING_FAIL` overwrites `ALARM_LIMIT_LATCHED`. Clear the Y failure and the
machine reads IDLE with Z0 still gated. The mask is what survives that; the
reason is only the headline.

**The transition is derived, never written.** The supervisor's success path is
not the only site that hardcodes IDLE — `cmdUnalarm` and `setorigin`'s
alarm-clearing block do too, so retracting Z0 while X is still latched would drop
to IDLE with no `unalarm` involved. All three go through one function:

```c
void resumeOrHold(void) {
    if (homingLatched) { alarmReason = ALARM_LIMIT_LATCHED; machineState = STATE_ALARM; }
    else               { alarmReason = ALARM_NONE;          machineState = STATE_IDLE;  }
}
```

Which means `unalarm` needs no special case for this reason: it clears what it
clears, calls `resumeOrHold()`, and the machine falls straight back into ALARM if
the physical condition is still there. One rule instead of three guards.

The invariant that makes it hold: **a bit is set on a seek's terminal verdict and
cleared only by that same axis's successful retract.** Never by `unalarm`, never
by `setorigin`, never wholesale. The mask is physical fact; the state is a view
of it.

#### The mask is node-framed, and lives in `position.h`

**This corrects the paragraph that used to stand here**, which put the mask in
`homing.h` as one bit per motion *slot*. Per slot it was wrong, and wrong in a
way that wedged the machine:

> Home Z on head 0 — slot 2, node 3 — which sets slot bit 2. Then `axis_map 1 2
> 5 6` binds slot 2 to node 5. Bit 2 now asserts that head 1's Z is sitting on a
> switch it has never touched, and since the mask gates `ALARM_LIMIT_LATCHED`,
> the machine holds in an alarm that `unalarm` cannot clear and `setorigin`
> cannot clear — only a physical retract on slot 2, or a reboot.

A limit switch is wired to a **node**. Whether it is held down is a fact about
that node's mechanism, and has nothing to do with which stream slot the node
currently occupies. So the truth is stored per bus id and the slot view is
re-derived on every bind — exactly the shape `nodeOrigin[]` / `nodeHomed` already
had, and for exactly the same reason. `position.cpp`'s own header comment states
the rule; this mask was written in violation of it.

```c
// position.h
void nodeLatchSet(uint8_t n, bool latched);   // node-framed truth
extern uint8_t homingLatched;                 // slot-framed view, derived
```

- `slotAdoptStatus()` re-derives the slot bit from the incoming node, so a bind
  **adopts** the latch rather than inheriting the outgoing node's.
- `slotUnbind()` clears the slot bit only. The node's bit is deliberately
  untouched: unbinding a slot does not move anything off a switch.
- `homingFail()` touches neither, and is already right in both failure modes — a
  seek that failed never reached the switch, and a retract that failed never
  escaped one.

The old reasoning for `homing.h` over `shared_state.h` — that sitting beside
`axes_homed` would overstate its authority, since a crash-latch during a job
never reaches this code (§6.6) — was sound but argued the wrong axis. `position.h`
answers it better anyway: the mask now sits beside the datum, the other
node-framed fact with the same caveat and the same rebind behaviour. `getstate`
includes `position.h` to report it.

**The generalisation, since this is the second time the same bug has been
written:** any fact about *physical mechanism* is node-framed. Any fact about
*the current stream* is slot-framed. Store the first per slot and it goes stale
on the next `axis_map`, silently, at the moment the operator is least expecting
state to change — nothing moved.

#### Reporting it: `latched=` on the text plane only

`getstate` gains one field, appended **last** so every existing parse position is
undisturbed:

```
state=0 enabled=0x00 homed=0x03 alarm=0 running=0 latched=0x00
```

`parseGetstate` ignores trailing tokens it does not know, so a new host reading
old firmware is a missing key rather than a parse failure.

**It is deliberately NOT in `STATUS_RSP`.** That frame is a fixed 30 bytes whose
length the demux checks, so adding a byte is a version-skew problem across two
binaries, not a field addition. The consequence is that `axesLatched` is
`undefined` on a binary poll — **not `0`**. Zero would be a claim ("every switch
clear") made on the strength of a frame that never asked, which is precisely the
class of silent-wrong this section exists to remove. The demo panel renders that
`undefined` as `— (binary poll — run getstate)`.

**Deploy the firmware and the host together.** `enumFromInt` coerces an unknown
`AlarmReason` to the fallback, so a host without `LIMIT_LATCHED: 6` renders an
alarmed machine as **NO ALARM** — it does not render `ALARM(6)`. The degrade is
silent and points the wrong way.

**Out of scope:** detecting a crash-latch during a job. Host preflight owns that
— see §6.6.

### 2.7 `span`: the node measures its own leg

**Implemented**, and it lives on the **node**, not the Pico. `homingArm()`
snapshots `absolutePosition`; `homingFinish()` stores `end - start`;
`node_status()` appends it to the stepper tail, and `nodestat <id>` prints it
as `span <steps>`.

It answers one question — **how far did the last completed leg actually
move** — and nothing past that.

**Per LEG, not per home, and that is what makes it answerable.** An earlier
design spanned a seek/retract PAIR and ran aground immediately: a full home is
*two* pairs, and the node cannot tell which one it is in, because it sees four
unrelated `CMD_HOME_LEG`s and has no sequence context at all. Per-leg is the
primitive the node can actually stand behind. Composing legs into "distance
from the far stop to the datum" is the master's job, and the master has the leg
boundaries to do it with.

**Why the node and not the Pico** — where it was first built, and where it was
wrong. The master *could* get the same number by bracketing two
`CMD_NODE_STATUS` reads around a leg, since `absolutePosition` is free-running
and nothing ever resets it (§4). What that argument misses is the **bench
path**: every value in §7 was found by driving one node directly with the raw
console `home` command, with no host sequencer in the picture at all.
Bracketing does not exist there. The node-side span does, which is exactly when
a homing diagnostic is most needed — bringing up an axis whose numbers are not
yet known.

Reading it is still the operator's job, and deliberately so:

- **Steps, never mm.** The Pico does not parse `stepsPerUnit` — that is host
  config, and a board that converted would be authoritative about a calibration
  it cannot check (§2.1). Signed, because the sign catches a leg that ran the
  wrong way.
- **It survives a failed leg, on purpose.** Where a leg stopped IS the
  diagnostic when it stopped somewhere unexpected — §7.2's seek died 164 mm
  into a 1200 mm frame, and that number is what disproved the truncation theory
  and found the real bug. An earlier version cleared the span on failure, on
  the reasoning that a failed leg's distance is just its budget. That was wrong
  twice: a leg that fails part-way travelled some OTHER distance, and that
  distance is precisely what the failure raises as a question.
- **Reading it as "max travel" is a precondition the node cannot verify.** The
  number is the frame's full extent only if the axis started the seek parked at
  the *opposite* hard stop. Nothing in the firmware can tell whether that was
  true; it reports what moved, not what that implies.

**The tail is now legitimately two lengths.** Switch-equipped boards send
`[pos][slot][span]` (9 bytes) and switchless ones `[pos][slot]` (5). Safe in
both directions of version skew because `nodeStatusDecode()` branches on
`len >= NS_STEP_LEN` rather than equality — an old master ignores the extra
four bytes, a new one reading a switchless node reports no span rather than
mis-parsing. `hasHomeSpan` carries the distinction, so a missing span never
degrades into a `0` that reads as "went nowhere".

**Why not a dedicated calibration command**, or a two-leg `home` wrapper that
seeks, backs off, and hands back a distance: it saves nothing. The host already
arms both legs of a real home, so such a command would be `home` then `home`
with the subtraction moved firmware-side, for a number the host can already
compute. The one thing it would buy — an answer in mm — needs `stepsPerUnit`,
and §2.1 keeps that on the host regardless of which command produced the steps.
That is the general shape of the bottleneck: any feature wanting the Pico to do
more than relay and count hits the same wall, because config reaches it only as
an opaque blob. Worth solving once, generally, if that class of feature is ever
actually wanted — not worth solving here for one subtraction.

---

## 3. Host

### 3.1 Schema

**Implemented** in `web/src/machine/schema.ts`, loaded by `json/load.ts`. The
shipped interface is this one plus the three fields the paragraph below adds —
`pullInFeed`, `rampSteps`, `parkMm` — nine fields in total.

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
`homePresent: false` flag. `buildHoming()` enforces this as all-or-nothing: a
partial block is an error, not a block with defaults. `atOrigin` in particular has
no default — there is no safe direction to guess.

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

**Three fields are missing, found by deriving §7's confirmed X/Y recipe back
through this schema.** `CMD_HOME_LEG` takes `start_us` AND `floor_us` AND
`ramp_steps`; `seekFeed` supplies only the second. The pull-in rate is a physical
property — the fastest rate the motor starts from rest without stalling — and is
not derivable from the cruise rate. And the two retracts are different distances
(X/Y use 2 mm for leg 2, 5 mm for leg 4), so one `backoffMm` cannot say both:

```ts
readonly pullInFeed: number;    // mm/s, the rate leg 1 STARTS at
readonly rampSteps: number;     // steps from pullInFeed to seekFeed
readonly backoffMm: number;     // leg 2 — must exceed RELEASE hysteresis
readonly parkMm: number;        // leg 4 — where the axis is left standing
```

The conversion in both directions is one expression:
`interval_us = 1e6 / (feed × stepsPerUnit)`. Checked against §7's X numbers
(`stepsPerUnit: 160`): `2500 µs` → 2.5 mm/s pull-in, `500 µs` → 12.5 mm/s seek,
`8000 µs` → 0.78 mm/s latch, `320` steps → 2 mm back-off, and the `88000` budget is
500 mm × 160 × 1.1 exactly. Z0 does not fit the same way: its `180000` is 150 mm
× 1200 with **no** 1.1 margin, so either its `hardTravel` is 136.4 or the margin
was skipped. Measure it rather than infer it.

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
to fill `CMD_HOME_LEG.dir` — as-is for a seek leg, inverted for a retract leg. The
node stores no direction of its own (§1.2).

Putting a raw approach dir in config instead would create two independent
direction facts that can disagree — an axis that streams one way and homes the
other, with nothing to catch it. The node never hears the word `invert`.

### 3.3 Validation

**Implemented** as one rule, `homingCoherent`, appended to `RULES` and hanging off
the existing `namedAxes()`. What shipped:

| rule | level |
|---|---|
| `hardTravel`, `pullInFeed`, `seekFeed`, `latchFeed`, `backoffMm`, `parkMm` all `> 0` | error |
| `rampSteps >= 0` | error |
| `latchFeed < seekFeed` | error |
| `pullInFeed <= seekFeed` | error |
| `seekFeed <= maxFeed` (when `maxFeed > 0`) | error |
| `parkMm < hardTravel` | error |
| `backoffMm <= parkMm` | warning |
| `hardTravel >= maxTravel` (when `maxTravel > 0`) | warning |

Three of these differ from the table this section originally carried, each for a
reason found while writing the rule:

- **`latchFeed < seekFeed` is an error, not a warning.** If the slow leg is not
  slower, leg 3 is not a re-approach and the entire two-pass structure buys
  nothing. That is a broken config, not a questionable one.
- **`pullInFeed <= seekFeed` is new.** A pull-in above the cruise makes the
  "ramp" a *decel*, so the axis meets the switch at the fastest point of the leg
  — the exact opposite of the intent.
- **`hardTravel >= maxTravel` is a warning, not an error**, and the comparison
  runs the other way round from the original row. Homing legitimately moves
  outside the soft envelope, because no datum exists yet to measure that envelope
  from. But `hardTravel < maxTravel` means one of the two numbers is simply
  wrong.

The two rows about `rotary` and about X/Y agreeing were not written: there is no
`RotaryHoming` member to conflict with yet, and an X-only machine is a legitimate
bring-up state that a warning would just train the operator to ignore.

The feed-ceiling rule is an **error**, unlike the house convention for targets.
`overCeiling` warns because targets get clamped; a seek feed above the ceiling is
clamped by nothing on the node — it stalls or overruns the switch.

`maxTravel` is untouched by this work. It remains the soft-limit envelope;
`hardTravel` is the physical extent between hard stops. They are different
numbers and both are needed.

### 3.4 The sequence

**Four** moves, then the datum. All host-side; the Pico gains no sequencer and the
node gains nothing at all — it is the same command four times with different
parameters.

**Implemented** in `web/src/homing/`, split on the seam that matters for testing:

| file | contents |
|---|---|
| `derive.ts` | **pure.** `approachDir()`, `derivePlan()` — config in, a four-leg plan plus the datum out. No link, no I/O, no clock. |
| `sequence.ts` | `runHoming()` — arms each leg, polls to its terminal verdict, then `setorigin`. Throws `HomingError`. |
| `types.ts` | `LegKind`, `HomingLeg`, `HomingPlan`. |

The split means the arithmetic every one of the paragraphs below argues about —
directions, budgets, the datum — is checked by tests that never open a link.
`SEEK_MARGIN = 1.1` and `LATCH_MARGIN = 2.5` are the two constants, the second
being the answer to "leg 3's budget must exceed leg 2's *actual* distance", below.

An earlier version of this section specified three, ending on the switch. That
leaves the axis latched, and the latch is cleared ONLY by a successful retract
(§1.1) — so a three-leg home ends with the machine unable to move, and under
§2.6 it ends in `ALARM_LIMIT_LATCHED`. The final back-off is not optional. §7's
bench-confirmed recipes have always had four legs; this section was the one that
disagreed.

Worked example — X, switch at the far end, 500 mm, 160 steps/mm:

```
home x 1 1000  125   2000 88000 0 → ok    # 1 fast seek: 1000µs→125µs over 2000 steps
                                          (poll; ends ALARM / LIMIT_LATCHED)
home x 0 1000  1000  0    320   1 → ok    # 2 back-off 2 mm — NOTE dir 0
                                          (poll; ends IDLE)
home x 1 20000 20000 0    8000  0 → ok    # 3 slow re-approach, no ramp
                                          (poll; ends ALARM / LIMIT_LATCHED)
home x 0 1000  1000  0    800   1 → ok    # 4 park 5 mm clear — NOTE dir 0
                                          (poll; ends IDLE)
setorigin x 79200                 → ok    # (500 mm − 5 mm) × 160 steps/mm
```

The trailing digit on each `home` is `intent` (§1.2's "The intent bit"): `0` for
legs 1 and 3, which are expected to start clear of the switch, `1` for legs 2
and 4, which are expected to start on it. It rides alongside `dir`, not instead
of it — get either one wrong against physical reality and this leg NAKs
(`nak intent_mismatch`) rather than running under the wrong leg's budget
semantics. This example predates the intent field; §7's bench-confirmed lines
below carry it too, added the same way.

**The datum goes last, and it is not `tripPos × stepsPerUnit`.** `homingRelease()`
invalidates the origin on EVERY home, success included, so a datum set after leg 3
is destroyed by leg 4. It must be set at the parked position:

```
pos_steps = (tripPos ∓ parkMm) × stepsPerUnit
```

That is exact, because of an asymmetry worth stating plainly: **a seek ends at an
unknown position — the trip point — while a retract travels exactly its budget**,
since it ignores the switch and nothing but the budget stops it (§1.3). Leg 4's
distance is therefore known a priori and leg 1's never is.

**Leg 3's budget must exceed leg 2's actual distance**, not the planned one — it
has to re-cross whatever leg 2 travelled to find the switch again. See §7.

**THE DIRECTION MUST ALTERNATE.** An earlier version of this example carried
`dir 1` on all three passes, and that is a crash: the node holds no direction
state (§1.2) and takes `dir` from the payload verbatim, so a retract sent with
the seek's direction drives *further into* the switch — and a retract ignores the
switch entirely (§1.3), so its budget is the only thing that stops it. Whichever
value reaches the switch, the back-off is its complement. Above, `1` is toward the
switch on X (bench-confirmed, §7) and the back-off is therefore `0`.

The `seek`/`retract` words are gone from the command as well — see §2.2.

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
Batched homing later is pure host sequencing over the same `CMD_HOME_LEG`, so neither
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

### 3.6 The operator surface

A Homing panel in `web/demo/comms.html` / `comms.js` — the demo, so the bar is
"exercisable and honest", not "production":

- **A checkbox per homeable axis**, meaning an axis with a `homing` block whose
  node is present. Absent config = absent row, which is the §3.1 rule made
  visible.
- **A homed tick per row**, repainted on every status sample *and* on `committed`.
  A Z/A on the non-engaged head reads `—`, not `·`: its slot currently holds the
  other head's node, so the mask has nothing to say about it, and `·` would be a
  claim the machine never made.
- **Dry run**, which needs no machine. It calls `derivePlan()` and prints the four
  legs and the datum. This is where a backwards `atOrigin` is caught — on a
  screen, rather than at seek speed against a hard stop — and it is the reason
  `derive.ts` is pure.
- **Home selected**, running axes **sequentially**: the firmware answers `err
  busy` to a second `home` while one is in flight, because Core 0 supervises
  exactly one at a time (§3.5).
- **Stop**, which is an e-stop and not a cancel. Mid-home the axis is between two
  known points; there is nothing to unwind to, and only a fresh home can say
  where it is.

The panel shows `ALARM` / `LIMIT_LATCHED` between legs, and says in a hint that
this is the sequence working (§2.6), not a fault. An operator who learns to clear
that alarm mid-home has been taught the wrong reflex by the UI.

**The Sim models `home`** (`wire/link/backends/sim.ts`), so all of the above is
exercisable without hardware: it arms, sits in `HOMING`, and finishes on the tick
loop, with seek-vs-retract decided the way the node decides it — one read of its
own switch at arm time (§1.2). It models the *protocol*, not the motion: arm,
poll, verdict, and the alternation of terminal states. Step timing is not
simulated, so it can prove the sequencer's logic and can prove nothing about
feeds.

---

## 4. What does not change

The datum machinery, the axis map and `ENGAGE`, the stream byte format, and the
MSEG path are all untouched. Homing bolts on beside them.

**In particular, `axis_map` does not cost a re-home.** This gets asked, because
the slot view of `axes_homed` visibly changes when a head is swapped. But the
datum is node-framed — `nodeOrigin[]` and `nodeHomed` are indexed by bus id — and
`slotAdoptStatus()` recomputes both `machinePos[s]` and the `axes_homed` bit from
the incoming node on every bind. Swap head 0 out and back in, and the axes that
were homed still are. Nothing moved, so nothing was lost; only the *view* was
rebuilt. §2.6's latch mask now works the same way, and the host refreshes status
after a commit so the panel shows the re-derived answer rather than the
pre-commit one.

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

The `CMD_HOME_LEG` payload, the pulser and its ramp, the non-blocking `home` command,
`STATE_HOMING`, the poll loop, the timeout derivation, `setorigin <axes>
<pos_steps>`, the `nodeOrigin` arithmetic, and the fault handling. All of it.

The budget becomes ~1.1 revolutions rather than `hardTravel × stepsPerUnit` — a
different number in the same field, not a different mechanism.

**That number was wrong here by 5.16×.** 8.890 steps/deg is the **motor-side**
rate; the budget has to be in the frame the axis actually turns in. At the
calibrated output rate of 45.8272 steps/deg, 1.1 revolutions (396°) is
**≈18,150 steps**, not 3520. A budget short by that factor reports a false
"index never found" on every attempt.

### 5.2 What genuinely differs

An index is not a barrier. There is no hard stop, the axis must keep rotating
during normal cutting, and blocking motion at the index would freeze A every
revolution. So:

- The **gate must be absent**, not merely unused, on a rotary node.
- Nothing **self-terminates** — the node must decide to stop.
- The **stopping position is irrelevant**; what matters is where the sensor
  asserted.

**The index is not a pin.** This section reads throughout as though the sensor
were a digital input like a limit switch. The A1324 is **ratiometric analog**: the
magnet produces a dip of roughly 735 samples across its arc, against a baseline,
and "asserted" is a threshold decision someone has to make in software. There is
no edge to read — there is a curve to find a feature in. Everything below about
"sampling the pin" holds only once that thresholding exists, and specifying it is
the actual unbuilt work.

The mechanisms converge more than that suggests. If the pulser samples the sensor
at the top of each step ISR and declines to step when asserted, then for a barrier
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
contrast, can be dropped — accuracy is set by per-step sampling rather than by
speed.

**But not to one step.** The ≈0.11° figure this originally quoted was one step at
the motor-side 8.890 steps/deg, mixing frames again. Edge detection against the
measured noise is worth about **5 steps**: the dip moves ~4.6 ADC counts per step
against ~23 counts of noise. Five steps at the output-side 45.8272 steps/deg is
≈0.11° — the same number, arrived at correctly, and it is a *sensor* floor rather
than a *step* floor, so finer microstepping does not improve it.

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
2. **Do not let `CMD_HOME_LEG`'s completion semantics assume "stopped because
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

- **`atOrigin` for X, Y, Z0, Z1** — which *end* each switch is at. Not a
  measurement so much as a look, but it is the one field with no safe default and
  no way to fail soft: backwards, the first seek runs a full `hardTravel` away
  from the switch and into a hard stop, at `seekFeed`. The values presently in
  `web/demo/comms.json` (`hardTravel: 500`, `atOrigin: false`) are placeholders,
  not findings. Dry run (§3.6) prints the derived direction and datum without
  moving anything; use it first.
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

### 6.6 A crash-latch during a job is invisible

§2.6 covers only latches the homing supervisor observes. A limit tripped by a
crash mid-job never reaches that code, so `homingLatched` stays clear while the
axis is gated, and the stream keeps emitting steps the node silently refuses —
the same divergence §2.6 exists to prevent, outside its reach.

Deliberately out of scope here: it is not a homing problem, and the host is
better placed to answer it with a preflight poll before a job than the Pico is by
polling continuously. Named so the gap is not mistaken for coverage.

---

## 7. Bench-confirmed reference

Values found by running the raw console `home <node> <dir> <start_us> <floor_us>
<ramp_steps> <max_steps>` command (§2.2's bare-minimum bench version) directly
against hardware, kept here so a future session does not have to re-derive them
from `comms.json` and re-discover the same corrections. These are per-node
findings, not spec — §3.4's derivation is still how a new axis gets a starting
point.

**The lines below predate the `<intent>` argument (§2.2, §1.2's "The intent
bit") and will NAK verbatim against current firmware.** Replaying any of them
today needs a trailing `0` on the two seek lines and `1` on the two retract
lines — `dir` and `intent` happen to coincide numerically in every line below,
which is a property of this particular bench sequence, not a rule; do not
assume they always match.

**Per-node direction and polarity, as confirmed on the bench:**

| node | axis | seek `dir` | `LIMIT_ACTIVE_HIGH` |
|---|---|---|---|
| 1 | X | **1** (see note) | yes — switch reads inverted from the active-low default |
| 2 | Y | 0 (see note) | yes |
| 3 | Z0 | 0 | no — default active-low is correct, unconfirmed against a mismatch report |
| 6 | Z1 | not yet probed | not yet probed |

**Nodes 1 and 2 (X, Y) four-leg sequence**, confirmed working on hardware —
the same numbers were used for both axes:

```
home <n> 0 2500 500  400 88000    # fast seek
home <n> 1 1000 1000 0   320      # retract
home <n> 0 8000 8000 0   800      # slow latch seek
home <n> 1 1000 1000 0   800      # final backoff
```

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
  This is a distance problem, not a universal verdict on `8000/8000` — X and Y use
  that same interval for their slow leg and it's fine there, because their retract
  distance (320–800 steps at 160 spu ≈ 2–5 mm) is much shorter than Z0's
  (5000 steps at 1200 spu ≈ 4.2 mm, crossed at 7.5× finer resolution per mm).
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

### 7.1 2026-08-31 run — supervisor bring-up

The first run of the §2 supervisor (`home` with an axis token, `homingTick()`,
`setorigin <pos_steps>`) against nodes 1 and 2. Confirmed working: both
directions, the switch-found path, the budget-exhausted failure path
(`STATE_ALARM` / `ALARM_HOMING_FAIL`), `err busy` on a doubled `home`, the datum,
and alarm recovery.

**Seek direction on node 1 now reads `1`, not the `0` in the table above.** A
`home x 1 ... 88000` found the switch and resolved to IDLE. The earlier row was
recorded with the bare bench command before the supervisor existed and has not
been re-confirmed since; the two have not been reconciled against the physical
wiring, so **probe direction with a small budget before trusting either** —
`home <axis> <dir> 2000 2000 0 500` moves half a millimetre and settles it. Node
2's row is untested since and carries the same caveat.

Three master-side defects found and fixed in the same run, all of them things the
bench command could not have exposed because it never polled the bus mid-move:

- `setorigin` answered `ok` for a mask in which **nothing** was bound, reporting a
  datum it had not recorded.
- A successful home cleared `STATE_ALARM` but left `alarmReason` at
  `ALARM_HOMING_FAIL`, so a recovered machine still read as broken — and the
  reason is what the host renders.
- `err busy` sat below the bus gate in `cmdHome`, which does not admit
  `STATE_HOMING`, so a second `home` mid-move got a generic `err bad_state` and
  the specific branch was unreachable.

### 7.2 2026-09-01 run — 1200 mm seek, wedge fix, and a node-side glitch

Two more master-side defects, found bench-testing X at `hardTravel: 1200` (up
from the placeholder 500), both fixed this session:

- **A stale `ALARM_ESTOP` wedged `STATE_HOMING` forever.** Stop mid-leg, then
  home again: leg 1 latched correctly, but the machine never left
  `STATE_HOMING` for leg 2 — `resumeOrHold()` is the only exit on that path and
  its guard excluded `ALARM_ESTOP`. Fixed by retiring `alarmReason` at the ARM,
  not the exit, so `STATE_HOMING` never coexists with a reason describing a
  machine that already stopped (§2.2, §2.6's `resumeOrHold`).
- **The node's homing pulser had no glitch rejection**, unlike the stream path
  (§1.1's `LIMIT_LATCH_BYTES`). A seek repeatably stopped ~164 mm into the
  1200 mm frame with `homefail=BUDGET` and `nodestat` reading `limit 0`
  immediately after — a transient assert halted the pulser, nothing latched,
  and the pin had released by the Pico's next 25 ms poll. `span` (§2.7) is
  what disproved the first theory (a 16-bit truncation at 14592 steps): the
  actual travelled figure, 26241, doesn't match, and pointed at the ISR
  instead. Fixed with a 3-sample debounce in the pulser, mirroring the stream
  path's discipline.

Both fixes are in `src/rp2350/core0/homing.cpp` and
`src/node/types/stepper/stepper.cpp` respectively — flash both, not just the
Pico, to get the second one.
