# Feed override — one dial for every unplanned stop

Status: **design, simulated only.** Nothing here has run on hardware. Every
number below comes from a Python model of the emitter; the simulation scripts
are named at the end.

This replaces the soft-abort ramp in `src/rp2350/core1/emit/microsegment.cpp`
and the constants in `src/rp2350/core1/motion_limits.h`.

---

## 1. The problem with the ramp we have

Today a stop is a velocity ramp. The emitter takes the current `interval`,
turns it into a velocity, and steps in the segment's direction ratios until the
velocity reaches rest. The code says what that costs:

```c
// Termination. Planned run: the segment's own step count. Ramping: rest
// — which can fall BEFORE or AFTER that count, so once the ramp is live
// the loop is no longer bounded by maxSteps.
```

Stepping past `maxSteps` means emitting steps the packet never contained. On a
straight jog that just travels a bit further. On a curve it leaves the path,
because "the segment's direction ratios" is a straight line and the path is not.

That single fact is why pause currently flushes the queue and forfeits the
rest of the stream, and why resuming needs the host to re-send everything.

---

## 2. The idea: turn one dial instead

Add one number, `s`, between 0 and 1. It is a speed dial — a feed override —
that multiplies the whole machine's speed:

```
emitted interval = planned interval / s
```

`s = 1` is full planned speed. `s = 0.5` is half speed: every tick takes twice
as long. `s = 0` is stopped.

That is the entire mechanism. Every unplanned stop is "turn `s` down to 0".
Every resume is "turn `s` back up to 1".

### Why it cannot overshoot

`s` changes **when** steps happen. It never changes **which** steps happen, or
**how many**. The Bresenham accumulators, the step order, the direction bits —
untouched. A packet still finishes after exactly its `T` ticks.

So the machine can slow to a crawl in the middle of an arc, stop, sit there,
and start again, and the steps that come out are bit-identical to the ones that
would have come out at full speed. Simulated over jobs containing six full
stops and five resumes:

```
steps emitted [420000, 240000, 0, 792000]
planned       [420000, 240000, 0, 792000]
delta         [0, 0, 0, 0]   EXACT
```

This is the property the whole design rests on. It is what makes the queue
worth keeping across a pause, and it is what makes `rampStepInBounds()`
unnecessary (§8).

---

## 3. How fast may the dial move? That is `K`

You cannot slam `s` from 1 to 0 — the motors would stall. Each motor has a
limit on how fast it can change speed. We need one number that says how fast
the *machine* can change speed.

`K` is **the fraction of its speed the machine can shed each second.**

A car doing 100 km/h that can slow by 20 km/h every second:

```
20 ÷ 100 = 0.2     it sheds one fifth of its speed per second
1 ÷ 0.2  = 5 s     so it stops in 5 seconds
```

`K = 0.2` per second. The *fraction* is what matters, not the absolute — a
bicycle at 10 km/h shedding 2 km/h per second has the same `K` and also takes
5 seconds.

For four motors, work out each one's fraction and take the **smallest** — the
sluggish one everybody else has to wait for. All four must slow together to
stay on the path, so the slowest one sets the pace.

Worked example, a real packet (`dx=35, dy=20, da=66`, lasting 14.35 ms):

| motor | steps | its speed here | can change by | fraction |
|---|---|---|---|---|
| X | 35 | 2,438 steps/s | 16,000 per s | 16000 ÷ 2438 = 6.56 |
| Y | 20 | 1,393 steps/s | 16,000 per s | 16000 ÷ 1393 = 11.5 |
| A | 66 | 4,598 steps/s | 22,992 per s | 22992 ÷ 4598 = **5.00** |

`K = 5.00` per second, so a full stop takes `1 ÷ 5 = 0.2 seconds`.

Two ingredients: *how fast is this motor going in this packet* (its steps ÷ the
packet's duration), and *how hard can this motor change speed*
(`maxAccel × stepsPerUnit`). Only one number is shipped — the minimum. The
other three are slack by definition.

### A stop is long

| packet | K | stop time | packets crossed |
|---|---|---|---|
| A-major arc | 5.00 /s | 200 ms | 57–63 |
| XY straight | 3.54 /s | 283 ms | 68–69 |

A packet lasts ~14 ms. **A stop spans 15–70 packets.** Fitting one inside a
single packet would mean decelerating 15× harder than the axis limit. So a stop
is always cross-packet — not a design choice, just arithmetic.

---

## 4. The one piece of real algebra: track `s²`, not `s`

We want `s` to fall at a steady rate in *real time*. But the emitter counts in
ticks, and ticks get longer as `s` falls — at half speed a tick lasts twice as
long. So "how much should `s` drop this tick" depends on `s`, which is awkward.

Squaring fixes it. If `s` falls steadily in real time, then **`s²` falls by the
same fixed amount every tick**, whatever `s` is:

```c
s2 -= 2 * C;          // C = K * interval / F_CPU
```

The stretching tick and the shrinking step cancel exactly. This is the same
trick the existing ramp already uses (`v2 = v*v - decel2`), and the same reason
the planner works in `v²`.

One subtraction per tick. No division, no square root, unless you need `s`
itself.

---

## 5. The ceiling: one rule, four reasons

Rather than deciding *when to start stopping*, ask every tick: **what is the
fastest I am still allowed to be going?** Take the lowest answer.

```c
float c2 = 1.0f;                                   // ceiling on s², start at full

if (pausing || cancelling)                         // operator pause, jog cancel
    c2 = 0.0f;
else {
    if (!endOfPathQueued)                          // queue starvation
        c2 = fminf(c2, K * fmaxf(0, queuedS - TAIL_S));

    if (dutyActive)                                // tool duty limit
        c2 = fminf(c2, sq(K * (deadlineS - nowS - TAIL_S - LIFT_S)));

    for (int i = 0; i < 4; i++)                    // soft limits
        if (d[i] > 0) {
            float head = hi[i] - pos[i] - R[i] * TAIL_S;
            c2 = fminf(c2, 2.0f * K * fmaxf(0, head) / R[i]);
        }
}
```

Where each line comes from — all four are "will I still be able to stop?":

- **Starvation.** Stopping burns queued time. The queue must outlast the stop.
- **Duty.** Stopping takes `s ÷ K` seconds; you have until the deadline.
- **Soft limit.** Stopping covers `R × s² ÷ (2K)` steps on each axis; that must
  fit in the headroom.
- **Pause / cancel.** No negotiation: the ceiling is zero.

Why some rules square `s` and one does not: the duty rule compares a *time*
against a *time*, so `s` appears once. The starvation and soft-limit rules
compare against something that itself shrinks as the machine slows, so `s`
appears twice. Nothing deeper than that.

### `s` follows the ceiling, slew-limited

The ceiling can drop abruptly — the queue suddenly runs short, a limit suddenly
comes into view. The machine must not.

```c
s2 = (s2 > c2) ? fmaxf(c2, s2 - 2*C)
                : fminf(c2, fminf(1.0f, s2 + 2*C));
```

`s²` moves toward the ceiling by at most `2C` per tick, in either direction. So
the accel limit holds no matter how violently the ceiling moves, and the same
line handles resuming: when the ceiling lifts, `s` climbs back at exactly the
rate the motors can take.

---

## 6. The reserve: why 20 ms keeps appearing

The ceilings above are **exactly** tight. Solve "how fast can I go and still
stop in time" and you get a value that stops you precisely at the deadline —
which means any rounding at all puts you past it.

Measured, with no reserve:

```
ARC   worst slack  -77.127 ms   late  64/200  *** UNSAFE ***
LINE  worst slack  -62.555 ms   late  63/200  *** UNSAFE ***
TINY  worst slack -155.787 ms   late  61/200  *** UNSAFE ***
```

A third of deadlines missed. Tightening a safety margin did not fix it
monotonically (0.95 was *worse* than 1.00), which is the signal that the cause
is structural, not slop.

**The cause is the last step.** As `s` approaches zero a tick takes
`interval ÷ (s × F_CPU)` seconds — the final ticks are enormous, ~4.7 ms each.
The machine does not crawl to zero, though: below `V_REST_SPS` it simply stops.
So the worst the tail can cost is one step at rest speed:

```
TAIL_S = 1 / V_REST_SPS = 1 / 50 = 20 ms
```

Subtract that from every deadline and they all pass:

```
ARC   reserve_tail=True  m=1.00  worst slack  +7.685 ms  late 0/300  SAFE
LINE  reserve_tail=True  m=1.00  worst slack  +8.764 ms  late 0/300  SAFE
TINY  reserve_tail=True  m=1.00  worst slack  +7.059 ms  late 0/300  SAFE
```

The same 20 ms is the reserve in all four rules. For the soft limit it becomes
a distance — `R_i × TAIL_S`, which at X's full rate is
`12,800 × 0.02 = 256 steps`. Without it the machine stopped exactly *on* the
limit; with it, exactly 256 steps short, at every headroom and every accel.

---

## 7. Test the ceiling every tick, not every packet

The most expensive bug found. Testing only at packet boundaries commits the
emitter to the whole packet once it enters one, so the error bound is not half
a packet — it is **one worst-case packet**:

```
packet durations:  median 15.66 ms   p99 135.01 ms   MAX 157.34 ms
result: over duty by 23.99 ms, with a ZERO-tick ramp
```

It blew the deadline without ramping at all. With a per-tick test:

```
mode=boundary  tightest  -23.994 ms  over 1  *** UNSAFE ***
mode=pertick   tightest  +15.680 ms  over 0  SAFE
```

The per-tick test costs a multiply, a subtract and a compare. The emitter
already pays a `sqrtf` and a float divide per step during a ramp today, so this
is cheaper than what is there now.

---

## 8. What this deletes

**`motion_limits.h` in full.** `DECEL_SPS2_*` is superseded by `K`, which comes
from real config instead of hand-seeded constants. The file's own header says it
is "scheduled for deletion". Two live defects go with it:

- `DECEL_SPS2_X = 160000` is 10× X's planning accel (`100 × 160 = 16000`), while
  `DECEL_SPS2_A` is 1× A's. `K` is derived per packet, so no such spread exists.
- `decelForAxis(majorAxis)` picks the decel rate by *major axis*. On an A-dominant
  block — normal for a knife below ~16 mm radius — that scales the ramp against
  the wrong axis. `K` takes the minimum across all four, so there is no major
  axis in the ramp at all.

**`rampStepInBounds()`.** Its stated reason to exist is:

> *Phase 2 of the ramp emits beyond the segment's planned delta, so it can cross
> a bound that the per-segment check already cleared.*

Feed override never emits beyond the planned delta, so there is no out-of-plan
step to check. The stub is deleted, not implemented — which also means the
config-read gap that blocks it does not have to be closed for this work.

Soft limits still need enforcing, but as a **ceiling** (§5), which is a
different and better thing: it slows down in time instead of alarming on
arrival.

---

## 9. The three unplanned stops

Same mechanism throughout. They differ only in what starts the ramp and what
happens after rest.

| | trigger | after rest | queue |
|---|---|---|---|
| **Starvation** | `s² > K·(queued − TAIL)` | wait, then ramp back up | **retained** |
| **Operator pause** | flag | park; host stops feeding | **retained** |
| **Jog cancel** | flag, jog's harder `K` | accept the new jog | **discarded** |

Jog cancel is the only one that discards the queue, and correctly — the next
jog command is a new intent, not a continuation. Position survives in all three:
`machinePos` is accumulated from what was *emitted*, not what was planned.

### Starvation does not chatter

Ramp down on a stall, ramp back up on refill. Reversal counts from the model:

```
one 50 ms stall     reversals  0    (a 64-packet queue absorbed it entirely)
one 500 ms stall    reversals 14
one 3 s stall       reversals 14    parked 2183.5 ms, resumed, steps EXACT
chattering 20x20ms  reversals  0
chattering 40x5ms   reversals  0
```

A 64-packet queue is ~900 ms deep, so ordinary feed jitter never reaches the
ceiling. Reversals appear only for stalls long enough to genuinely empty it.

It is also self-stabilising: as `s` falls, the queue's real-time content grows,
so slowing down buys the host more time to catch up.

### Starvation needs the end-of-path flag

Without it the design **cannot finish a job.** The last packets of a path
legitimately drain the queue to empty, so the starvation ceiling forbids
executing them and the machine creeps toward a stop it never reaches:

```
parked 20000.5 ms, steps [-29, -57, -7, -58]   (deadlock)
```

`MSEG_FLAG_*` has spare bits. When a path-end packet is in the ring, the
starvation rule is suspended — the host planned that stop.

### Pause never finds itself fast with an empty queue

A worry that turns out to be answered by the starvation rule:

```
512 pkts queued  ->  stopped after 228.81 ms   queue retained
 32 pkts queued  ->  stopped after 228.81 ms   queue retained
  8 pkts queued  ->  stopped after 125.99 ms   queue retained
  2 pkts queued  ->  stopped after  53.99 ms   queue retained
  1 pkt  queued  ->  stopped after   0.00 ms   queue retained
```

A short queue means the starvation ceiling has *already* slowed the machine, so
the stop pause needs is already short. The dangerous combination — high speed,
nearly-empty queue — is exactly the state the starvation rule makes
unreachable. The two rules compose without needing to know about each other.

### Jog cancel and soft limits

```
headroom 200000  ->  stopped at 199744   margin 256 steps   violations 0
headroom    400  ->  stopped at    144   margin 256 steps   violations 0
   (with a 4x harder jog accel: identical, margin 256)
```

Constant margin, because the reserve is a fixed travel time, not a fraction.

---

## 10. Tool duty as a Pico responsibility

Duty is *"N seconds powered, then M seconds off"* — a wall-clock property of the
tool, not a geometric property of the path. The host can only ever predict it.
The Pico can count it:

```
poweredUs = queuedUsOut - toolOnMark
```

`queuedUsOut` already exists as a monotonic counter of consumed segment time.
Powered time is a subtraction on it — no estimation, no accumulated error, no
dependence on planned-versus-actual.

With the duty ceiling in §5, the cycle is:

1. Ceiling descends through `s`, machine ramps to rest — anywhere, including
   mid-arc, because override cannot leave the path.
2. Lift Z, cut tool power, mark the cooldown deadline.
3. Report the pause; the host stops feeding. **The queue stays.**
4. Deadline passes: power on, dwell for spin-up, lower Z, ramp `s` back to 1,
   continue from the exact tick where it stopped.

Measured over full repeated cycles (30 s duty, 10 s cool, 0.5 s spin-up,
0.1 s each Z move):

```
window  1  powered  29.9743s  (margin +0.0257s)  cutting 29.2743s =  97.7%
window  2  powered  29.9871s  (margin +0.0129s)  cutting 29.2871s =  97.7%
...
steps emitted [420000, 240000, 0, 792000]
delta         [0, 0, 0, 0]   EXACT
```

Across ~9,000 simulated powered windows (4 packet shapes × 4 duty settings,
16 random seeds × 3 settings): **no window ever exceeded the limit**, tightest
margin +7.6 ms, always early, and steps exact on every job that ran to
completion.

Utilisation falls with the window — 97.6% at 30 s, 92.8% at 10 s, 76.3% at 3 s —
because the 0.7 s of fixed overhead is per cycle. That is a tool spin-up
property; nothing in the planner can recover it.

### A feasibility precondition

Below a duty window of ~0.72 s the fixed overhead alone exceeds the budget and
the tool is over duty before it moves. No ramp can fix that. Check once at job
start and refuse:

```
SPINUP + Z_LOWER + Z_LIFT + TAIL_S  <  DUTY
```

---

## 11. What has to change

**`K` rides in the packet, from the host.** Neither core parses config —
Core 0 handles the config blob as opaque bytes (length, CRC32, stage, commit)
and never reads a field. The host already computes `interval` from `maxFeed`,
`stepsPerUnit` and `maxAccel`, so it is the only place that can compute `K`.

Cost: one float, `MSEG_PACKET_SIZE` 26 → 30. That touches the frame-size
constants, which is the widest blast radius in the change.

Jogs use the same field with a harder accel constant (`jogAccel` rather than
`maxAccel`), so jog feel is a config number rather than a firmware `#define`,
and Core 1 never needs to know a jog is a jog.

**`s_rest` costs nothing.** The fastest axis is the major axis, which steps once
per `interval`:

```c
s_rest = V_REST_SPS * interval / F_CPU;    // no config needed
```

**An end-of-path flag** in `MSEG_FLAG_*` (§9).

**Jog routing.** Core 1 admits jogs during pause:

```c
runningReason = RUNNING_JOG;    // only jogs are accepted during pause
```

They land in the **same `masterBuf`**. Harmless today because pause flushes
first; once the queue is retained, jog moves interleave with suspended job
packets in one FIFO and the job resumes with jogs spliced into it. Core 0
already discriminates at ingest via `MSEG_MAGIC` / `JOG_MAGIC`, so routing jogs
to a separate small ring is cheap — but it has to land *with* queue retention,
not after.

**`resumePos` becomes a gate.** For a tool swap the operator jogs away, so the
retained queue is only valid at the stop position. Resume should refuse unless
`machinePos == resumePos`.

**Duty config** pushed once at job start: limit, cooldown, lift height, spin-up
dwell — the same channel as soft limits.

---

## 12. What is not established

- **Nothing has run on hardware.** All of it is simulation.
- **Thermal model.** Counting powered seconds is exact; whether the tool's real
  limit is "N on, M off" with no partial recovery is a tool question.
- **Nested pauses.** Operator pause during a duty cooldown needs a small state
  machine, not a flag.
- **Z headroom at the stop point.** A duty stop lifts Z wherever it lands; that
  lift has to be feasible.
- **Jog accel is still unmeasured.** The suspicion that `DECEL_SPS2_X` is not
  step-safe is untested. What changes under override is *which* risk: an
  over-hard decel can no longer desync the emitter's arithmetic, only outrun the
  motor physically. The bench test is unchanged and still worth doing — home,
  jog back and forth cancelling mid-move N times returning to the same nominal
  position, re-home, compare `datumSteps`. Zero means clean.
- **The starvation ceiling is ~2× conservative.** The exact queue needed to stop
  from `s` is `s² ÷ (2K)`; the rule uses `s² ÷ K`. Tightening it would recover a
  little throughput on a starved link, but it has not been tested tight.

---

## 13. Simulation scripts

In the session scratchpad, not committed:

| script | what it establishes |
|---|---|
| `duty2.py` | steps exact across a stop and resume |
| `duty3.py` | the naive ceiling is unsafe — 64/200 deadlines late |
| `duty4.py` | the tail is the cause; 20 ms reserve fixes it |
| `duty5.py` | repeated duty cycles, utilisation |
| `duty6.py` | stress: mixed shapes, random queues, short windows |
| `duty7.py` | isolates the zero-tick ramp failure |
| `duty8.py` | per-tick ceiling; full sweep passes |
| `unplanned.py` | all three unplanned stops together |
| `starve.py` | starvation diagnosis — rules out `K` choice as the cause |

One methodological note: the starvation breach was *not* caused by the choice of
`K`. Head-packet, queue-minimum and machine-wide minimum all failed identically,
which is what ruled that explanation out and pointed at the exact-tightness
problem instead.
