# Tool-height probe

Status: **design, not implemented.** Nothing in §8.1 has been measured, and §8.1
decides how much of this document survives. Read that section before building
anything here.

---

## 0. The decision

Descend Z until a bed-floor NC switch opens, stop, report the machine-frame Z at
contact. The switch is wired to the **vacuum** node, not to Z.

The shape is homing's — a config-defined sequence of legs, a Core 0 supervisor
that arms and returns, a node-framed budget — but the execution model is a job's,
because of one fact that drives everything else in this document.

**No hardware changes.** The switch stays on the vacuum. That constraint is what
makes this hard, and every design decision below traces back to it.

---

## 1. Why this cannot be homing

A home is executed by the node. `CMD_HOME_LEG` arms the stepper's own pulser; the
node reads its own limit pin, stops itself, and the Pico only polls for
completion. The axis that moves and the switch that stops it are the same node,
so the control loop never touches the bus.

A probe cannot work that way. Z's node has no visibility of the bed switch, and
the vacuum cannot drive Z. **The control loop has to cross the bus**, which means
the Pico closes it, which means the Pico is emitting motion — a job-shaped
activity, not a homing-shaped one.

Three consequences that shape the rest:

1. Motion is Pico-emitted, so it lives on Core 1 as an emit path alongside
   `emit/debug_step.cpp`, not in the node.
2. The bus is in the servo loop, so its latency and jitter enter the measurement.
3. While Core 1 is emitting, `rpcServerPoll()` is starved
   ([core1.cpp:88](../src/rp2350/core1/core1.cpp) services it *strictly after*
   the queue drain, deliberately — "a bus transaction takes up to
   RESPONSE_TIMEOUT_MS, which is many step intervals"). So the supervisor cannot
   poll anything mid-leg. Every cross-check happens **between** legs.

---

## 2. Lockstep

### 2.1 The mechanism

The Pico does not emit the next stream byte until it has received the reply to
the last one.

The request is free: it rides the stream byte already being sent, as the poll
bit. The reply is one 12 µs frame. Nothing else changes about the wire.

### 2.2 Why this dissolves the timing problem

The obvious design — leave a fixed quiet window, resume on a timer — has a hard
real-time constraint, because a late reply lands on a transmission that was
scheduled regardless. Lockstep has no window to miss. A slow vacuum does not
collide; it slows the step rate.

That converts the failure mode from **collision** (corrupt motion, silent) to
**stall** (no motion, obvious). A stall is caught by the poll deadline (§5.7) and
fails the leg cleanly.

It also gives a guarantee the windowed design cannot: the latency between
switch-open and last-step is bounded by exactly one round trip, **by
construction**, because the Pico structurally cannot have emitted a step it has
not heard back about.

The Pico can hear the reply because it is not transmitting during it. This is
load-bearing and it is measured, not assumed: `/RE` is strapped to `DE` on the
Pico board, so the receiver is deaf while transmitting. Proven by contrast on
this bench — 5,000,000 bytes transmitted with zero received and zero framing
errors, against 40 words received cleanly while silent. Lockstep is the only
reason that strapping is survivable here.

### 2.3 Feed is a ceiling, and the achieved feed is an output

The bus clocks the steps, so the configured feed cannot be a target. The emitter
takes `interval = max(configured_interval, round_trip)`: faster bus, honour the
config; slower bus, the bus wins.

Homing never needs to report achieved feed — the node's pulser hits its
programmed interval exactly. A probe does. A configured 2.5 mm/s that the bus
actually delivered at 0.9 is silent fiction, so **achieved feed (or min/mean/max
step interval) is part of the probe result**, not a diagnostic afterthought.

The ceiling is bus-capped: two frames per polled step against ~83,800 bytes/s
means a 40 µs round trip supports roughly 20 mm/s and a 100 µs one about 8.

### 2.4 Ramping survives, and is not optional

Lockstep sets a *floor* on the step interval; it does not dictate a schedule. So
`interval = max(ramp_interval(s), round_trip)` accelerates normally — early
intervals are far longer than any round trip, so the bus is not the binding
constraint until top speed.

Keeping the ramp is what protects the measurement. Step count is exact by
construction; the only way a probe loses position is a motor that **skips**
because it was commanded above its start-from-rest rate. At 500 mm/s² a 20 mm/s
approach ramps over 0.4 mm — real distance, and the leg that travels furthest is
the one that needs it.

---

## 3. The stream byte

### 3.1 What a stream byte means, now that a non-stepper participates

Until now the stream was master→steppers only. It needs a definition a third
party can reason about:

> **A stream byte with all four step bits (0, 2, 4, 6) clear is a guaranteed
> motion no-op.**

This is already load-bearing in shipping firmware — it is why the `0x000` sync
preamble before every command reply and `busQuiesce()`'s NOP byte are safe. Every
node-originated stream byte must satisfy it.

Nodes already transmit stream bytes (`sendCommandPacket()` sends that preamble),
so "only the master streams" is already false. The extension is that one such
byte carries a nonzero bit.

### 3.2 Prerequisite: DIR must not latch on a non-stepping byte

**Landed** on `refactor/core-boundary` (`4e6f40b`), independently of the probe
as this section asked. Before it, [stepper.cpp](../src/node/types/stepper/stepper.cpp)
latched DIR *before* checking the step bit, so a zero-step byte still drove DIR
and still paid `delayMicroseconds(5)` inside the RX ISR — the invariant in §3.1
was not true. The stream ISR now returns on `(b & stepBitMask) == 0` before it
reads the dir bit. Still wants regression on a real job; it is the hot path on
every stepper.

This has already caused one failure. The NOP preamble reads as "direction 0",
drove DIR low, and yanked it out from under the homing pulser mid-leg; the fix
was a targeted `if (homingActive) return;`, which patches one caller rather than
the class.

The general fix:

```c
bool stepReq = (b & stepBitMask) != 0;
if (!stepReq) return;                   // no step → do not touch DIR
bool newDir = (b & dirBitMask) != 0;
```

Behaviour-preserving: `microsegment.cpp` computes `dirBits` once per segment and
ORs it into **every** byte, so any byte that steps axis *i* already carries axis
*i*'s correct dir bit. Direction is never sent in a separate leading byte by any
emitter. The only change is that a reversal pays the 5 µs guard on the stepping
byte rather than one byte earlier.

**Land this independently of the probe.** It converts an almost-true invariant
into a true one and retires a class of bug that has already cost one homing run.
It touches the hot path on every stepper, so it needs regression on real jobs.

### 3.3 Reply encoding, fail-safe by construction

The vacuum replies with a stream byte carrying switch state in its own slot's
**dir** bit, all step bits clear.

Encode so the *safe* state requires positive assertion:

| dir bit | meaning |
|---|---|
| 1 | switch still closed — keep going |
| 0 | switch open, or unknown |

Then a corrupted byte, a dead vacuum, and bus silence all read as "stop". Failure
is safe by construction rather than by handling.

### 3.4 A stream byte has no CRC

This is the one uncorroborated read in the design, and it has two consequences.

A bit flip that sets Z's step bit (bit 4) injects a phantom step: 0.83 µm of
divergence between the Pico's dead reckoning and Z's counter. Detectable at the
next leg boundary (§5.8), and small.

A bit flip in the reply's dir bit reads as a spurious trigger or a missed one. A
spurious trigger costs a retry (§5.9). A missed one is corrected on the next poll
— 333 µs later on a latch leg.

Neither is tolerable as the *final* word, which is why contact is confirmed over
the CRC-protected command path before it is believed (§5.9).

---

## 4. Node

### 4.1 The vacuum gets its own `CMD_ENGAGE`

Slot state (`slot`, `stepBitMask`, `dirBitMask`) plus one `CMD_ENGAGE` handler is
roughly 40 lines and could move into `dispatch.cpp` as core state. It should not.

The vacuum's engage genuinely differs from the stepper's: no position to report,
no dir bit consumed for motion, and the binding means "answer polls here" rather
than "step here". That is a different command wearing the same opcode. Unifying
handlers that are about to diverge is the wrong move; the knife would want a
third variant.

The ack reuses `buildNodeStatus()`, whose first byte is `node_type()` — which
makes §5.3's type verification free.

### 4.2 The reply comes from the RX ISR

Stream bytes reach the vacuum's ISR
([isr_generic.cpp](../src/node/rs485/isr_generic.cpp) currently drops them on the
floor). Replying from there rather than from `loop()` is the whole latency
argument: `loop()` runs the SSR burst-fire machine, a variable-duration task
sitting directly in the reply path.

The ATtiny3226 has **no hardware XDIR** — DE is a plain software-toggled GPIO. No
ownership fight with the USART, and all the turnaround jitter is in firmware
where it can be controlled. Check reentrancy against the SSR machine.

A reply the node cannot deliver in time must be **dropped, not sent late.** Under
lockstep a dropped reply costs one poll; a late one collides with the Pico's
timeout path. The node checks elapsed-time-since-RX before asserting DE.

### 4.3 The switch pin is pulled up, but weakly

`pinMode(HAL_VACUUM_SWITCH_PIN, INPUT_PULLUP); // NC switch → GND`. Closed pulls
to ground, open rises. An instantaneous sample is well-defined — the line does
not float.

But an AVR internal pull-up is 20–50 kΩ, which makes a long run to a bed switch a
high-impedance node. This machine has already been bitten by exactly that: a
homing seek stopped dead at 26241 steps because the pulser halted on a single
port read and a motor cable coupled into the switch line. Expect noise; see §5.9.

### 4.4 No debounce on the trigger edge

Bounce on contact looks like open, spurious close, open. **The first open is the
true surface.** Any debounce — N consecutive reads, or a sticky latch with a time
threshold — deliberately waits before believing the switch, and that wait is
depth, at exactly the moment depth is being measured.

The asymmetry that settles it:

- A noise trigger stops Z early. The confirm sequence finds the switch closed and
  the leg retries. **Cost: time.**
- A debounce filter delays *every* trigger including the true one. **Cost: depth,
  on every probe, unconditionally.**

So: trigger on the first open, immediately, unfiltered. Reject noise after
stopping, where rejection is free (§5.9). There is deliberately **no latch** on
the vacuum, which also avoids the stale-latch failure mode that would make the
next leg trigger on step one.

**This is a deliberate divergence from homing, not an oversight.** The homing
pulser debounces — `HOMING_LIMIT_SAMPLES` consecutive asserted reads before it
believes the switch ([stepper.cpp](../src/node/types/stepper/stepper.cpp)) — and
that debounce arrived as a fix, not as a design. Homing can afford it because a
home is not a measurement: the datum *is* whatever the seek returns, so a few
steps of extra latency move the origin without anyone being able to call it
wrong. A probe measures against an origin that already exists, so the same
latency is error, not offset.

What pays for the divergence is the **zero-step confirm poll** (§5.9). Rejection
does not have to happen inside the sampling loop, because once Z has stopped the
Pico can interrogate the same pin through the same code path with Z's step bit
clear. That confirm is statistically stronger than any N-of-M filter the ISR
could run, and it costs nothing but ~700 µs of standing still. Homing's pulser
has no equivalent — it is free-running, it never stops to ask, and there is no
master in its loop to ask on its behalf — so it filters where it samples because
that is the only place it can. Same conclusion in both, *believe the switch only
after corroboration*, placed on the side that can pay for it.

---

## 5. Pico

Three commands, one session:

| command | §  | effect |
|---|---|---|
| `probe_map <stepper-id> <switch-id>` | 5.3 | enter the session, rebind slots |
| `probe_leg <dir> <ceil_us> …` | 5.7 | run one leg |
| `probe_end` | 5.5 | exit, restore the saved axis map |
| `axis_map …` | 5.5 | exit, committing a new map instead |

### 5.1 `STATE_PROBING`

A probe is a **session** spanning several legs, not one atomic operation.
`runningReason` is "meaningful only while RUNNING", so between legs — where no
motion is happening but the probe binding is still live — no reason code can
describe the machine. A session needs a state.

The gap is genuinely dangerous. During a probe binding X/Y/A hold no slots.
`ALARM_LIMIT_LATCHED` is justified on the grounds that "a job admitted here would
run three axes and silently drop the fourth"; a probe binding is that condition
inverted and worse — a job admitted between legs would move Z and silently drop
three.

A new `MachineState` is refused by all three relevant gates with **zero edits**:

| gate | site | why it refuses |
|---|---|---|
| MSEG | `data_plane.cpp` | admits `IDLE`/`RUNNING` only |
| JOG | `data_plane.cpp` | admits `IDLE`/`PAUSED`/continuing-jog only |
| `axis_map` | `busGateDenies()` | admits `IDLE`/`PAUSED`/`ALARM` only |

That default-deny is the entire argument for a state over a reason. A
`RUNNING_PROBE` reason would need each of those amended by hand, and would still
not cover the between-legs gap.

Entry from `IDLE` or `PAUSED`; return to whichever it came from, following
`returnState` in `microsegment.cpp` ("a jog during pause returns to PAUSED — the
job is still suspended"). A mid-job tool swap is that case exactly.

### 5.2 Reasons: activity, and the one phase fact the Pico owns

Homing does not track which leg it is on — the host issued the command, so it
knows. What homing *does* give the host is a leg-done signal, via the
HOMING→IDLE transition. A session state destroys that signal, so it has to be put
back.

The distinction to encode is **executing a leg** vs **between legs**, not
SEEK/LATCH/RETRACT. It is mechanically real: during a leg Core 1 holds the emit
position and `rpcServerPoll()` is starved; between legs it is not. So the reason
marks precisely when the bus is available for supervision — the only window in
which §5.8's position cross-check and §5.9's command poll can run.

**Three reasons:**

| reason | means | bus | admissible next |
|---|---|---|---|
| `PROBING_LEG` | a leg is executing; Core 1 holds the emit position | starved | nothing |
| `PROBING_CLEAR` | between legs, switch closed — tool off the surface | available | any leg, or exit |
| `PROBING_CONTACT` | between legs, switch open — tool on the surface | available | a retract leg only |

The names follow `RUNNING_JOB` / `RUNNING_JOG` — a reason carries its state's
prefix. None mentions seek, latch or retract: the Pico does not know which leg of
four it is running, and it never learns.

But it does know the switch, and the switch **partitions the legs**. Open means a
retract is the only thing that can legally happen next; closed means a descent
can. That is not leg identity smuggled back in — it is coarser, two-valued, and
derived rather than remembered — but it is real phase information, and it is
information the Pico owns rather than infers.

**Derived on read, never latched.** `PROBING_CLEAR` / `PROBING_CONTACT` are not
two states the session transitions between; they are one state rendered through a
fresh read of the pin at the moment the reason is asked for. This matters, because
it is what stops the third reason from being a second source of truth: it is the
*same read* the intent gate already performs (§5.7 — "the Pico can read the switch
itself between legs, so it checks before arming"), not a copy of it kept
alongside. A latched phase flag set at leg end would drift the instant a tool
slipped or a cable twitched; a derived one cannot.

Between legs the bus is available, which is precisely what makes the read
affordable — the same fact that motivates the `LEG` / not-`LEG` split in the first
place. During a leg the switch also has a state, but Core 1 holds the emit
position and nobody can ask, so there is deliberately no `PROBING_LEG_CONTACT`.

Two things fall out of exposing it, and both were previously obligations with
nowhere to live:

- `probe_end` in `PROBING_CONTACT` is refused, not quietly honoured. Exiting there
  restores the axis map and leaves the tool pressed into the bed. It reports
  `NOT_CLEARED` (§5.10).
- A console operator can see what the machine is sitting on without issuing a
  command. The host has the same fact from the last leg result (§5.11); the
  operator, mid-bail-out, does not.

### 5.3 `probe_map <stepper-id> <switch-id>`

A **full alternative binding**, not an overlay. Two commands writing one slot
table is the "updated one frame, forgot the other" class that
[axis.cpp](../src/rp2350/core0/cmd/axis.cpp) says cost a 1000-line file once
already.

1. Save the committed axis map.
2. Disengage every bound node. Each `CMD_ENGAGE` ack carries `node_type()` as its
   first status byte, so the disengage pass **verifies types for free**: the id
   called a stepper is a stepper, the id called a vacuum is a vacuum.
3. Refuse to proceed if any node fails to ack.
4. Engage Z to slot 2, vacuum to its slot.

Step 3 is the safety property, not a nicety. The Pico requests a poll by setting
the vacuum slot's **step** bit — so a stepper still engaged in that slot would
take one step per poll, a phantom axis tracking Z's entire descent. Verified
teardown makes that impossible by construction.

### 5.4 Which slot for the vacuum

Z takes slot 2 (`SLOT_Z`). The vacuum takes **slot 3**.

Weak preference, since §5.3 removes the hazard that would make it matter: on
builds with no fourth axis, slot 3 is the one most likely genuinely unoccupied,
and if teardown verification ever regressed, phantom steps on a rotary/aux axis
are less destructive than on the gantry. Slot 1 would be fine.

### 5.5 Exit

A probe session has no natural end. The last leg cannot know it is the last —
the host owns the sequence (§5.7) — so the exit is an explicit command rather
than something the Pico infers.

**Two commands exit, funnelling to one teardown:**

- `probe_end` — "put it back the way it was." Needs no arguments, so it works
  from a console and in the bail-out case, where the operator is already unsure
  what state things are in.
- `axis_map …`, admitted in `STATE_PROBING` — "here is the binding, and that ends
  the probe." For a host that ends a session by committing a new binding, this
  saves a round trip and the window in between where the machine is bound to
  nothing useful. A host that exits with `probe_end` never needs it — the route
  exists so that committing a map is never *refused* in `STATE_PROBING`, not
  because it is the expected way out.

Admitting `axis_map` is not an overload; it is the `ALARM_CONFIG` parallel below
taken seriously. Any committed map ends the session, and `probe_end` is sugar for
committing the one that was already there.

Exit-by-flag on `probe_leg` was considered and rejected. `max_steps` cannot be
zero (§5.7), so there is no no-op leg — an exit flag would make **motion
mandatory for teardown**, which is exactly backwards for the case where you most
want to bail. It would also move the teardown's ~160 ms of blocking bus work
(§5.11) out of a command handler and into the supervisor tick.

Either route must save `returnState` (IDLE or PAUSED) at `probe_map` time;
neither can infer where the session started.

Binding a vacuum into a motion slot **pollutes the position model**. `slotBind()`
writes `machinePos[s]`, `axes_homed` and `homingLatched` for whatever slot is
bound; the vacuum has no stepper tail, so the slot lands at position 0 with the
datum cleared, while `reconcileValidity()` still projects `nodeEnabled` through
the map into `axes_enabled`. The host would see a slot reading as an enabled,
unhomed axis at zero.

So exit disengages everything and **replays the saved ids through `cmdAxisMap`**
— the existing path, not a restore routine. That path is "deliberately dumb, not
a diff" and rebuilds every one of those fields from ENGAGE acks rather than from
anything remembered, so it is correct even if a node reset mid-probe. A second
binder restoring from saved state is precisely where this would go wrong.

The parallel is `ALARM_CONFIG`: during a probe the machine genuinely has no
working axis map, and the way out of that condition has always been to commit
one.

### 5.6 The emit path

A new Core 1 emit path beside `emit/debug_step.cpp`. It must accumulate
`machinePos[]` exactly as `processMicroSegments` does (Core 1 is sole owner), and
it must replicate `returnState` including the `jobActive` term — two copies of
that logic is a drift risk worth watching.

It must also wire in the estop, on the same terms as every other emit path.
§5.11.1 already says a stop during a probe is `ESTOP` → `ALARM_ESTOP` with the
datum voided; this is the path that has to make it reachable. Lockstep makes the
point sharper than it is elsewhere: this emitter's normal condition is *blocked
waiting for a vacuum reply*, so a stop must be observable inside that wait. If it
is only checked between replies, a stop issued against a node that has gone quiet
waits out the poll deadline (§5.7) before anything happens — the machine would sit
still, which is safe, but it would sit still for the wrong reason and report the
wrong one.

### 5.7 `probe_leg` — arming one leg

```
probe_leg <dir> <ceil_us> <ramp_steps> <poll_div> <max_steps> <deadline_us> <intent>
```

Positional, `err usage` / `err range`, following `lin_leg`'s idiom in
[axis.cpp](../src/rp2350/core0/cmd/axis.cpp). The host owns the sequence and the
Pico runs one leg — there is no leg index, and the Pico does not know which leg
of four this is.

**No node argument.** `lin_leg` is node-addressed because a home is node-framed
and runs before any map is committed. A probe is the opposite: the session
already bound both nodes (§5.3), so naming them again would be a second source of
truth that could disagree with the binding.

| arg | meaning |
|---|---|
| `dir` | 0/1, as `lin_leg` |
| `ceil_us` | step interval floor — the feed **ceiling** (§2.3) |
| `ramp_steps` | 0 = no ramp; required on fast legs (§2.4) |
| `poll_div` | poll every N steps; 1 = every step |
| `max_steps` | budget; `BUDGET` failure when exhausted |
| `deadline_us` | poll deadline for **this leg** (§5.7 note below) |
| `intent` | 0 = expect switch closed at start, 1 = expect open |

Zero is rejected for `ceil_us`, `poll_div`, `max_steps` and `deadline_us` — same
reasoning `legCommon` gives for rejecting a zero interval and a zero budget: a
command that cannot move and cannot fail.

**The intent check runs on the Pico, not the node.** `CMD_HOME_LEG` pushes it to
the node (`NAK_INTENT_MISMATCH`) because only the node can see its own limit pin.
Here the Pico can read the switch itself between legs, so it checks before
arming. Same purpose — catch a sequencing error before it drives an axis for a
full budget — implemented on the side that can see.

**`intent` is not made redundant by `PROBING_CLEAR` / `PROBING_CONTACT` (§5.2),**
even though the two read the same pin. The reason reports what the machine has;
`intent` is the host declaring what it believes. Catching a host that disagrees
with the machine is the whole value of the check, and a check derived from the
machine's own reading can never disagree with it. This is already the homing
design's position — `CMD_HOME_LEG` carries an intent the node could otherwise
infer from its own pin, and carries it precisely so that it can be contradicted.
Do not later "simplify" this by computing `intent` from the reason.

**Refuse a de-energised Z**, reading `nodeEnabled` directly as `legCommon` does.
Its rationale transfers verbatim: a de-energised node accepts the leg and the
motor cannot turn, so the terminator is never reached, the budget burns out, and
the result reads as a broken switch rather than a motor nobody turned on.

### 5.7.1 The legs

Four legs, homing's shape. Poll density is a **per-leg parameter**, because crash
protection and measurement have latency requirements three orders of magnitude
apart:

| leg | feed | poll every | overtravel on trip | purpose |
|---|---|---|---|---|
| approach | fast, ramped | ~64 steps | ~50 µm | do not crash |
| retract | — | — | — | clear the switch |
| latch | 2.5 mm/s | 1 step | ~1 µm | **measure** |
| retract | — | — | — | clear the switch |

On non-poll steps the Pico emits and continues without waiting; on poll steps it
waits. The resulting ripple is one ~40 µs stretch every 64 steps — irrelevant on
a leg that is not measuring.

There is **no Z start offset in config.** Tool length varies by tool, so no
standoff height can be computed in advance, and a tool that slipped in its holder
violates whatever bound was assumed. The approach leg starts from wherever homing
left Z and finds its way down under sensing. This is why the approach cannot be
an ordinary microsegment move: MSEG is refused in `STATE_PROBING`, and an
unsensed rapid is the crash the probe exists to prevent.

The poll deadline is per-leg and **not** the bus's `RESPONSE_TIMEOUT_MS`. 20 ms is
fine for the latch leg but at 20 mm/s it burns 0.4 mm before stopping even
begins, which eats most of the switch's ~1 mm of travel.

### 5.8 Position: two counters that must agree

Checked before the first leg and at every leg boundary — the only windows where
`rpcServerPoll()` runs (§1).

`CMD_GET_POS` from Z against the Pico's dead reckoning. A mismatch means steps
were refused — and the limit gate refuses **silently** — so everything downstream
is measuring a fiction. Mismatch fails the probe.

The Pico must keep `machinePos[]` updated throughout, because the probe runs
after homing and position validity has to survive it.

### 5.9 Confirming contact

On first open, stop. Then, with Z stationary:

1. **N zero-step stream polls.** Z's step bit clear, vacuum's poll bit set. Same
   code path, one bit different. ~40 µs each, and **zero depth cost because
   nothing is moving** — which is what makes real statistical rejection
   affordable here and unaffordable during descent (§4.4).
2. **One `CMD_SWITCH_GET`.** The stream reply has no CRC (§3.4); the command path
   does. This is the only corroborated read in the design, and a contact
   declaration should not rest on an unchecked byte.

Total ~700 µs, once, after motion has already stopped. Still open → real contact.
Closed again → noise; retry the leg.

Bound the retries. Exhaustion is its own failure code — "the switch is
chattering" points somewhere completely different from "never found the surface".
**Report the retry count even on success**: a probe that succeeds on the fourth
attempt every time is telling you something, and an intermittent fault that never
quite fails is otherwise invisible from the bus.

### 5.10 Failure taxonomy

Split by *where to look*, following `HOMEFAIL_*`:

| code | meaning | suspect | Z datum |
|---|---|---|---|
| `BUDGET` | ran the budget, never contacted | tool length, bad travel figure, dead switch | intact |
| `POLL` | vacuum stopped answering | the bus; the motion may have been fine | intact |
| `CHATTER` | retry limit exhausted | switch or cable noise | intact |
| `ALREADY_OPEN` | switch open at arm time | switch failed, or Z parked on the bed | intact |
| `NOT_CLEARED` | retract done, switch still open | tool still on the bed, or switch failed open | intact |
| `POS_MISMATCH` | node counter ≠ dead reckoning | steps refused; measurement void | **void** |
| `DEADLINE` | supervisor timeout | the emitter, not the switch | **void** |

**Most probe failures do not destroy the datum**, and the existing code already
draws that line correctly: [position.cpp](../src/rp2350/core0/position.cpp) keys
origin invalidation on `STATE_ESTOP || ALARM_ESTOP || ALARM_SOFT_LIMIT`, not on
`ALARM` generally. A probe-fail alarm must not use either of those reasons.

`POLL` is homing's own reasoning transplanted — `HOMEFAIL_POLL` says "the bus is
the suspect. The motion may well have been fine." Lockstep makes it stronger
here: the Pico physically cannot have emitted a step it did not get a reply for,
so a poll failure stops at a count it knows exactly.

Only the two marked **void** lose the datum, and for the same reason in both
cases — an unknown number of steps went unaccounted for.

### 5.11 Terminal states

**Success: no alarm.** `ALARM_LIMIT_LATCHED` exists because *the node refuses
stream steps while latched*, so a job admitted there would silently drop an axis.
That mechanism does not exist here — the vacuum gates nothing. And the retract
leg leaves the tool clear with the switch closed, so there is no hazardous
condition to describe. An alarm would also break the case that matters most: a
mid-job tool swap must land back in `PAUSED`, and an alarm would force an unalarm
before the job could resume.

Derive the terminal state from a post-retract switch read; do not assign it.
`resumeOrHold()` makes this point for homing — derived, never assigned, because
per-leg assignment is correct only until legs interleave. Here it means
`NOT_CLEARED` cannot be reported as success by a path that forgot to check.

**A leg boundary is not a terminal state.** Legs 1 and 3 end in nothing
observable at state level: the machine stays in `STATE_PROBING` and the reason
falls back to `PROBING_CLEAR` — or `PROBING_CONTACT`, if that leg was the one
that found the surface (§5.2). That transition *is* the leg-done signal,
and restoring it is the whole reason the reason exists.

What every leg boundary does produce is a result — stop cause, steps taken,
resulting `machinePos[Z]`, switch level, retry count (§5.9) — reported, not stored
(§6.2). After the first approach leg it reads "contacted, at depth D, 0 retries";
the host issues the retract; and nothing in the machine's state records that a
probe is half-finished. It cannot. The host owns the sequence, so "half-finished"
is a fact only the host holds, and the Pico inventing a way to express it would
put the leg index back in by the back door.

The terminal states below are the *session's*, reached by §5.5's exit or §5.11.2's
teardown — never by a leg completing.

### 5.11.1 Two recovery ladders, and they are not the same

These are separate severities and must not be collapsed. Only the second
de-energises, and de-energising is the only thing here that destroys a datum.

**Probe failure → `ALARM` + probe-fail reason**, mirroring `ALARM_HOMING_FAIL`.
Nothing is de-energised, so the datum survives except in the two cases marked in
§5.10. Recovery is `unalarm`, fix the cause, retry. **No re-home.**

**Stop during a probe → `ESTOP` → `ALARM_ESTOP`.** The estop sweep de-energises
the whole bus, which makes every axis back-drivable and voids every datum, so
recovery is re-home then re-probe. `core1.cpp` also clears `jobActive` — "any
suspended job is unrecoverable" — so a stop during a tool-swap probe kills the
job too, not just the probe. Existing behaviour, newly reachable from a paused
tool swap. Confirm that is acceptable rather than discovering it.

**There is no pause for a probe.**

### 5.11.2 A failed leg tears the session down itself

Disengage, then replay the saved map (§5.5), then enter `ALARM`.

The reason is not `setorigin` — the datum usually survives (§5.10). It is
`unalarm`: without a restore, clearing the alarm would return the machine to
`IDLE` with a probe binding live, three axes holding no slots and a vacuum in
slot 3. That is the silent-axis-drop condition. The invariant is that **`ALARM`
is never entered with a probe binding live**, the same shape as the estop sweep's
guarantee that "once you observe ALARM, everything on the bus is already parked."

The restore is **best-effort**. If the bus is what failed, some engages will time
out — but `cmdAxisMap` already handles that (`parkForget(n)` with no answer,
`slotBind` from acks), so a partial restore is the same defined degradation any
`axis_map` produces on a flaky bus, not garbage.

**Capture the probe failure reason before restoring, and do not let the restore
overwrite it.** Otherwise a `POLL` failure whose restore also fails reports as a
config problem, and the diagnostically useful fact — the vacuum stopped answering
— is gone.

This path does its ~160 ms of blocking bus work inside the supervisor tick rather
than a command handler, which [homing.h](../src/rp2350/core0/homing.h) otherwise
warns against. That is a deliberate asymmetry, not an oversight: a failed probe is
already a stop-everything event with nothing streaming, so an unresponsive
control plane during a fault is tolerable in a way it would not be on the happy
path — which is exactly why the happy-path exit is a command (§5.5).

---

## 6. Host

### 6.1 Config

Homing's shape, minus a Z start offset (§5.7), plus the binding and the poll
divisor. Feeds in mm/s converted to intervals host-side, as homing does.

The switch's X/Y location on the bed is machine geometry and belongs in config.
There is no Z counterpart.

### 6.2 The result is reported, not stored

The Pico exposes contact position (machine-frame steps), achieved feed, retry
count and failure code, held until the next arm — `homingFailWhy()`'s lifecycle.

Comparing this tool's contact height against the previous tool's and shifting the
remaining segments is arithmetic the host is already doing, since the host
generates the microsegments. A tool table on the Pico, or an offset the Pico
silently applies to incoming motion, would be the first piece of job semantics to
live there — and it would be invisible to the thing planning the moves.

---

## 7. What does not change

- The command protocol. `CMD_SWITCH_GET` and `CMD_ENGAGE` already exist.
- The 9-bit framing, the baud rate, the packet layout, CRC.
- Broadcast semantics. Nothing here is broadcast; the invariant that no node
  answers a broadcast is untouched.
- Homing. Its node-side pulser, its legs, its failure codes are all unaffected.
- Normal job streaming, apart from §3.2's DIR fix, which is behaviour-preserving.

---

## 8. Open

### 8.1 Numbers to measure before first run

**These decide how much of this document is needed.** Every timing figure above
is arithmetic plus an estimate of software latency; none has been on a scope or a
counter.

1. **Vacuum reply latency, `loop()` path — mean and spread.** The spread matters
   more than the mean. If it is tight, the whole slot-reply mechanism (§3, §4.1,
   §4.2, §5.4) is unnecessary and an ordinary `CMD_SWITCH_GET` poll suffices —
   collapsing this design to config, one emit path and one state.
2. **Vacuum reply latency, RX-ISR path** — and whether the turnaround is
   achievable at all against the SSR machine.
3. **Switch bounce duration, and separately switch repeatability** — trip-point
   scatter over many approaches. That figure sets the floor on probe precision
   and may well dominate everything the bus contributes. If it is ±10 µm, the
   difference between a 40 µs and a 300 µs poll is below the noise and the
   simplest mechanism wins by default.
4. **Switch line noise while the gantry moves.** Cheap, and it decides whether
   §5.9's retry path is insurance or the common case.

The measurement is not parameter-fitting. It decides whether §3.2's DIR fix is a
prerequisite or optional hygiene, and whether §5.3–§5.5's slot machinery exists
at all.

**Measurements 1 and 2 are per-board, and there are now two vacuum boards.** The
switch this document polls lives on whichever one is powered, and they are not
interchangeable for timing purposes:

| | ATtiny3226 (on the bus today) | AVR128DB32 (in development) |
|---|---|---|
| probe switch | PA3 | PA2 |
| clock | 16 MHz internal | 24 MHz crystal |
| RS485 DE | software toggle | hardware XDIR (PF3) |

Both share `NODE_ID` 7 and are mutually exclusive on the bus, so a measurement is
always of one of them and never of "the vacuum".

The DE row is the one that matters here, and it cuts deeper than the clock. On
the ATtiny, `HAL_RS485_TX_BEGIN()` is `digitalWrite` plus `delayMicroseconds(10)`,
with a further microsecond before release — roughly 11 µs added to every reply,
and, worse for §2.2, added *by software running inside the reply path*. §4.2 puts
that reply in the RX ISR, so on the ATtiny the ISR must hold DE across a busy-wait
while the SSR machine is also asking for the CPU. That is a jitter source with no
equivalent on the AVR board, where the USART drives DE itself, hardware-timed. The
24 MHz crystal then scales everything else by 1.5× on top.

So the AVR board should measure materially tighter on both 1 and 2 — which is a
trap, not a comfort. Measuring on the AVR and concluding the slot machinery is
unnecessary would be a conclusion about a board that is not on the bus. Measure
the ATtiny first, because it is both the current hardware and the worse case; if
its spread is tight enough to collapse §3–§5, the AVR's is too, and that
conclusion is safe in a way the reverse is not.

### 8.2 Undecided

- Whether an unrecognised `RunningReason` degrades silently in the web host, as
  an unrecognised `AlarmReason` is known to (it renders as "no alarm"). Affects
  nothing here directly — this design uses a state, not a reason — but the same
  decoder handles both.
- Retract distance, and whether it is per-leg or one constant.
- Retry limit for §5.9.
- **Reason precedence when a failure restore lands an empty map.** `cmdAxisMap`
  re-enters `ALARM_CONFIG` on an empty commit, which would collide with the
  probe-fail reason §5.11.2 says to preserve. An unconfigured machine is the more
  urgent gate; the probe failure is the more useful diagnosis. Not resolved.

### 8.3 Deferred

- Probing any axis other than Z.
- Probing against a switch on a node other than the vacuum.
- Automatic tool-table maintenance. §6.2 keeps that host-side deliberately.
