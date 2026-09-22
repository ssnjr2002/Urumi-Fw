# Bus Alarm — node-initiated stop

**Status:** design. Nothing here is implemented. §5 (the `FERR` check) is a
standalone bug fix that stands on its own and should land regardless of whether
the rest is ever built. Every timing figure below is *computed from the PIO
program and the baud rate*, not measured — see §11.
**Cross-links:** [homing.md](homing.md) (§1.1, the stream gate this reports on),
[comms_architecture.md](comms_architecture.md) (framing, ACK/NACK),
[wire_protocol.md](wire_protocol.md) (host↔Pico), [node_type_architecture.md](node_type_architecture.md)
(the per-type RX ISRs), `src/rp2350/core1/bus/uart_9bit.pio`.

> **Revision note.** An earlier draft of this document proposed detecting the
> alarm as a **UART break**, and proposed patching the RX state machine's `push`
> instruction between blocking and non-blocking modes. **Both are retracted.**
> Break detection does not work while the Pico is transmitting (§2.2), and the
> instruction patching is unnecessary once the RX FIFO is drained in the emit
> loop (§3.1). The mechanism is now **readback-compare**. Break detection
> survives only for the idle case, which is deferred (§10).

---

## 0. The problem

**A node that cannot do what it was told has no way to say so.**

The bus is single-master by construction: the Pico initiates every transaction
and nodes speak only when addressed. During a stream that becomes a blind spot,
because the Pico transmits continuously and never asks anything. A node in
trouble has no turn to speak in and no mechanism to take one.

This is not hypothetical. [homing.md](homing.md) §1.1 states it outright — a limit
trip during a job is *"blocked at the node, not merely reported."* The node
silently refuses every stream step while its switch is asserted. The Pico keeps
streaming, keeps adding those steps to `machinePos`, and the two frames diverge
with no observable symptom until someone notices the part is wrong. The same hole
covers a TMC2660 `DRV_STATUS` fault, a thermal trip, and a detected stall.

The constraint shaping everything is that there is **no spare conductor** — the
RS-485 pair is the only path between Pico and nodes, so the alarm must travel on
a bus that is already busy and already has exactly one authorised transmitter.
§9 records what we would do instead if a wire existed.

### 0.1 Why the probe forces the issue

The tool-height probe hands the bus to the vacuum node for the duration of a
drive. During that window the Pico is not the master. Since **the bus is the only
stop path on this machine** — the estop is `sendBroadcast(CMD_DISABLE)` plus a
serial sweep, with no hardware contactor — an unstoppable drive is not an
inconvenience, it is the absence of an estop.

Node→Pico alarm and Pico→node estop-during-probe turn out to be **the same
mechanism with the roles swapped**. Building it once serves both (§4).

### 0.2 Scope

**This document covers the streaming case only.** Detecting an alarm while the
Pico is idle needs a different mechanism, works for different reasons, and is
deferred to §10. Everything in §2–§7 assumes the Pico is actively transmitting.

---

## 1. Background — how the 9-bit UART actually works

The alarm is not a protocol feature. It is a consequence of how the PIO UART is
built and cannot be evaluated without knowing that. Skip to §2 if the PIO program
is already familiar.

### 1.1 What PIO is, and why it is here

The RP2350 has three PIO blocks, each with four state machines. A state machine
runs a nine-instruction ISA out of a shared 32-instruction memory, one
instruction per cycle, deterministically — no interrupts, no cache, no jitter.

We use it because **the RP2350 has no hardware 9-bit UART**. The bus needs a 9th
bit to separate command frames from stream bytes, so the UART is synthesised in
PIO: one state machine for TX, one for RX.

Two PIO features carry most of the weight:

- **Delay slots.** `[n]` after an instruction stalls `n` extra cycles, so
  `jmp x-- bitloop [6]` costs 7. This is how bit timing is built with no timer.
- **Side-set.** `.side_set 1 opt` steals encoding bits to drive a pin *at the same
  time* as the instruction's own action.

### 1.2 The configuration trick

Three instruction fields are mapped to two different pins:

```c
sm_config_set_out_pins(&c, pin_tx, 1);      // OUT  → TX
sm_config_set_sideset_pins(&c, pin_tx);     // SIDE → TX
sm_config_set_set_pins(&c, pin_de, 1);      // SET  → DE   ← the surprise
```

So in the TX program **`set pins, …` drives DE**, while **`out pins, …` and
`side …` drive TX**. Read `set pins, 1` as *"assert DE"*, never as *"write to TX"*.

### 1.3 The clock

```c
float div = (float)clock_get_hz(clk_sys) / (8 * baud);
```

Each state machine runs at exactly **8× the baud rate**, so **one bit = 8 PIO
cycles**. At `RS485_BAUD` = 921600 one PIO cycle is **~136 ns**, one bit ~1085 ns.

### 1.4 TX

```
idle:
    set pins, 0 side 1      ; DE low, TX high (idle)          1 cycle
    pull                    ; block until a word is in the FIFO
    set pins, 1 [5]         ; DE HIGH — driver on             6 cycles
    set x, 8   side 0 [7]   ; TX low = START BIT; x=8         8 cycles = 1 bit
bitloop:
    out pins, 1             ; next OSR bit → TX               1 cycle
    jmp x-- bitloop   [6]   ;                                 7 cycles  } 8 = 1 bit
    nop        side 1 [7]   ; TX high = STOP BIT              8 cycles = 1 bit
```

`jmp x--` branches while `x != 0` and decrements either way, so `out pins, 1`
executes **9 times** — nine data bits, LSB first.

Frame total: 8 + 72 + 8 = **88 cycles = 11 bit-times ≈ 11.9 µs**.

Two source-comment defects worth correcting in place:

- **The DE setup comment is wrong by 20×.** `set pins, 1 [5]` is annotated
  *"~40ns"*. 40 ns is 6 cycles at the 150 MHz system clock, but the SM is divided
  to 7.37 MHz (§1.3), making it **~814 ns**. Harmlessly conservative, but wrong.
- **DE glitches low between back-to-back bytes.** The wrap passes through
  `set pins, 0` and `pull` before re-asserting, dropping DE for ~2 cycles
  (~271 ns). Real frame-to-frame spacing is **~96 cycles ≈ 13 µs**, and that is
  the figure to use for duty-cycle math — giving a bus ceiling of ~77k bytes/s.

### 1.5 RX

```
start:
    wait 0 pin 0        ; stall until RX goes LOW = start bit edge
    set x, 8    [10]    ; 11 cycles — skip the start bit, land inside bit 0
bitloop:
    in pins, 1          ; sample RX into the ISR         1 cycle
    jmp x-- bitloop [6] ;                                7 cycles  } 8 = 1 bit
    jmp pin good_stop   ; RX high here? → valid stop bit
    irq 4 rel           ; RX LOW → FRAMING ERROR: raise the flag
    wait 1 pin 0        ; resynchronise: wait for the line to go idle
    jmp start
good_stop:
    push                ; hand the word to the FIFO
```

Two properties matter later:

- **`push` is blocking, and is reached only on a well-formed frame** — the
  framing-error path never touches the FIFO. So the sole stall condition is "a run
  of valid frames filled the 8-deep FIFO." §3.1 keeps that from happening.
- **A framing error pushes nothing.** Corrupted frames therefore manifest as
  *missing bytes*, which is what makes the shortfall check in §3.1 work.

`irq 4 rel` resolves to `4 + sm_num`, and flags 4–7 are **not NVIC-routable** on
RP2040/RP2350 — poll only. That is fine and in fact preferred (§3.1).

---

## 2. Why break detection does not work during a stream

### 2.1 The idea

Have the alarming node hold the line low for longer than one frame. Since every
legal frame has a high stop bit at least every 11 bit-times, a continuous low
beyond that **cannot be data**. This is exactly how DMX512 and LIN delimit frames
(§8), and a six-instruction PIO state machine can measure it directly.

### 2.2 Why it fails — contention is not a break

**The claim "a continuous low cannot be data" is about detection, and says
nothing about whether a collision produces that condition. It does not.**

Both drivers are symmetric push-pull with low output impedance (RS-485 requires
≥1.5 V differential into 54 Ω, so each sources tens of mA through single-digit to
tens of ohms). When they oppose:

- Line A: the Pico pushes toward Vcc through ~10 Ω while the node pulls toward
  GND through ~10 Ω → **A sits near mid-supply**
- Line B does the same in mirror image
- **A − B ≈ 0 V differential**

The RS-485 receiver threshold is ±200 mV, so 0 V is squarely in the **undefined**
band. What any given receiver outputs depends on which driver is marginally
stronger, on fail-safe bias, and on position along the cable. It may resolve
high, resolve low, differ between nodes, or chatter near threshold.

So during contention the line is **not low — it is undefined**, and the break
detector's premise fails precisely when the Pico is transmitting. DMX works
because its break is driven by the *only* transmitter on the link; ours would be
generated by a collision. **DMX validates the detection mechanism thoroughly and
the generation mechanism not at all.**

### 2.3 What survives: readback-compare

**The Pico knows what it sent.** It does not need the line to resolve to any
particular value — only for the readback to differ from the transmission, which
under contention it will, whichever way the indeterminacy falls.

With `/RE` tied active on every board, the receiver stays live during
transmission, so every transmitter already reads back the actual bus state. That
loopback is the whole mechanism.

---

## 3. The mechanism

### 3.1 Pico side — drain and compare

Every byte sent must come back. Track sent bytes in a small ring; pop one per
received byte and compare.

```c
#define LB_RING     8      // in-flight is 1–2; 8 is generous
#define LB_LAG_MAX  4      // comfortably above genuine in-flight depth

static uint16_t lbRing[LB_RING];
static uint8_t  lbHead, lbTail;
static bool     lbFault;

static inline void lbReset(void) { lbHead = lbTail = 0; lbFault = false; }

// immediately after rs485.writeStream(streamByte):
lbRing[lbHead++ & (LB_RING - 1)] = streamByte;

while (rs485.available()) {
    uint16_t got = rs485.read();
    if (lbTail == lbHead) { lbFault = true; break; }   // more back than sent
    if (got != lbRing[lbTail++ & (LB_RING - 1)]) lbFault = true;
}
if ((uint8_t)(lbHead - lbTail) > LB_LAG_MAX) lbFault = true;   // shortfall
```

Three independent failure modes, all covered:

| symptom | cause |
|---|---|
| **wrong content** | contention flipped bits |
| **shortfall** | framing errors push nothing (§1.5), so bytes vanish |
| **excess** | somebody else's frame landed on the bus |

Contention produces at least one. The residual — garbage coincidentally equalling
the sent byte — is 1-in-512 for a 9-bit value and collapses to nothing across a
multi-byte alarm.

**Cost: ~15–25 cycles.** A FIFO status read, a FIFO read, a compare, two ring
indices. The minimum step interval is ~1950 cycles at 150 MHz (from the ~13 µs bus
ceiling), so this is **~1%**, in a loop already polling `machineState` twice per
step.

Two consequences fall out for free:

- **No `push noblock`, no instruction patching.** Draining every step keeps the
  FIFO near-empty, so the blocking `push` never stalls and the RX SM never stops
  watching the line. The earlier §3.2 proposal is deleted.
- **Alignment is guaranteed**, because blocking push never drops anything. This is
  why the two properties must go together: `noblock` would break alignment.

Call `lbReset()` wherever `flushRX()` is already called at stream start.

The framing-error flag (§1.5) remains available as **corroboration, not the
primary detector** — a real event raises both, so disagreement between them is
itself diagnostic. Poll it in the same place; do not renumber the PIO program,
since an NVIC interrupt mid-step is exactly the jitter `__time_critical_func`
exists to avoid.

### 3.2 Node side — drive the line LOW

```c
// ~3 byte times at 921600 baud
HAL_USART_INST.CTRLB &= ~USART_TXEN_bm;   // release TX from the USART
digitalWrite(PIN_PA1, LOW);               // drive it low ourselves
digitalWrite(HAL_RS485_DE_PIN, HIGH);     // now put it on the bus
delayMicroseconds(36);
digitalWrite(HAL_RS485_DE_PIN, LOW);      // release the bus first
digitalWrite(PIN_PA1, HIGH);              // back to idle level
HAL_USART_INST.CTRLB |= USART_TXEN_bm;    // USART owns the pin again
```

Order matters: TX low *before* DE high, and DE low *before* restoring TX, so the
wrong level is never driven onto the bus. `RXEN` is untouched, so reception
continues throughout.

**Caller context.** The stepper's stream gate lives in the RX ISR, so
`delayMicroseconds(36)` blocks reception for ~3 byte times there. Acceptable in
this specific case — the node is refusing those steps anyway, which is why it is
alarming — but it needs a comment, because it will not be obvious why an ISR is
allowed to spin. Alarm sources found in loop context (a `DRV_STATUS` poll) have
no such concern.

### 3.3 Rejected alternatives

**Assert DE without driving (DE-only).** With DE asserted and the USART idle, the
TX pin idles **high**, so this drives the bus to *mark* — the same state as idle.
It contests only the Pico's low bits, which works mid-frame. But the bus is
saturated only at maximum step rate; at 1000 sps the line is idle ~99% of the
time, and **a 36 µs assertion into an idle bus does nothing at all.** The alarm
would be reliable at full speed and invisible during slow moves. Rejected.

**Transmit three `0x000` bytes.** Less code, but `0x000` carries a proper **high
stop bit**, so a byte landing in a gap is received as a perfectly valid stream
byte of value zero — no framing error. It would still be caught as an *excess*
byte, but detection becomes subtler and rests on count bookkeeping rather than a
hard framing violation. Holding the line continuously low gives a guaranteed
violation instead, for six extra lines.

It is also the safest thing for the other nodes: uncontested, they read `0x000`,
a stream byte with **every step bit clear** — a no-op. The alarm's failure mode is
"no motion", which is the direction we want.

---

## 4. The probe case — when the vacuum is the transmitter

During a tool-height probe the vacuum node emits stream bytes to drive a Z node.
Roles swap: the vacuum is the transmitter that needs to detect interference, and
the Pico is the one that needs to interrupt.

### 4.1 The vacuum's echo check

Simpler than the Pico's, because the vacuum has exactly **one byte outstanding**
at a time — no ring, no lag window:

```c
static volatile uint8_t lbExpect;
static volatile bool    lbPending;

// in the pulser ISR, right after transmitting the stream byte:
lbExpect = streamByte; lbPending = true;

// in the RX ISR, while a probe drive is active:
uint8_t status = HAL_USART_INST.RXDATAH;
uint8_t b      = HAL_USART_INST.RXDATAL;

if (probeActive) {
    if (status & USART_FERR_bm) { probeAbort(); return; }  // corrupted frame
    if (!lbPending)             { probeAbort(); return; }  // nothing was outstanding
    if (status & 0x01)          { probeAbort(); return; }  // 9th bit set = command frame
    if (b != lbExpect)          { probeAbort(); return; }  // wrong content
    lbPending = false;                                     // clean echo — consume it
    return;
}
```

`probeAbort()` clears TCA0 and stops emitting — the same halt path `CMD_DISABLE`
already uses.

**REJECTED: "abort on any RX outside my own TX window."** An earlier draft
proposed this. It is **racy**. `RXC` fires when the stop bit completes —
essentially the same instant as `TXC` — but `HAL_RS485_TX_END()` does
`delayMicroseconds(5)` *before* dropping DE. The node's own echo therefore arrives
while DE is still asserted, by a margin that depends on interrupt latency.
Sometimes inside the window, sometimes outside. The rule would abort the probe on
the node's own transmission, intermittently. Comparing content has no such race.

### 4.2 The Pico's estop during a probe

The Pico sends its **normal broadcast `CMD_DISABLE`**. No special jam path.

| Pico sends | lands in a gap | collides with a vacuum byte |
|---|---|---|
| `CMD_DISABLE` broadcast | clean frame, 9th bit set, `lbPending` false → **abort**, *and* every node receives a real estop | corrupted → `FERR` or mismatch → **abort** |
| break (line low ~36 µs) | start bit + zeros + low stop bit → `FERR`, `lbPending` false → **abort** | echo corrupted or missing → **abort** |

The broadcast is the better move: in the ~98% case where the line is idle it both
aborts the drive *and* genuinely disables every node. In the collision case the
drive still aborts, and the Pico re-sends into the now-quiet line.

Latency: detection within ~12 µs of the Pico starting to transmit, then the pulser
stops on the next tick — **roughly one step**, sub-micron at probe feeds.

### 4.3 AVR-specific caveats

**ISRs do not nest on AVR.** The pulser ISR blocks ~22 µs per step (5 µs DE guard
+ 11.9 µs byte + 5 µs release). An `RXC` firing inside that is deferred until the
pulser returns. Ordering is preserved and the `RXDATA` FIFO is 2 deep, so nothing
is lost at probe rates, but worst-case abort latency gains one byte time — still
1–2 steps.

**`FERR` (§5) is load-bearing on this path**, not merely prudent.

### 4.4 The Pico's independent watchdog

The Pico cannot compare against intent during a probe — it is not transmitting.
But it knows the *shape* the vacuum's bytes must have: **only the bound Z slot's
step/dir bits set, every other slot's bits zero.** Any byte violating that is
corruption or a misbehaving vacuum.

Combined with counting bytes against the step budget, this is supervision that
survives the vacuum itself misbehaving — the one failure `max_steps` at the
vacuum cannot contain, since it lives on the node it is meant to bound.

---

## 5. `FERR` is not checked anywhere — land this first

**A standing bug, independent of everything else here.**

Both RX ISRs read `RXDATAH` and test **only bit 0** (`DATA8`, the 9th bit).
Neither checks `FERR` (bit 2) or `BUFOVF` (bit 6):

- `src/node/types/stepper/stepper.cpp` — `ISR(HAL_USART_RXC_vect)`
- `src/node/rs485/isr_generic.cpp` — same shape, all non-stepper types

So **a frame with a bad stop bit is accepted as valid data today.** On a stepper
that means line noise, a reflection off an unterminated stub, or a marginal DE
turnaround can be decoded as a stream byte and **inject steps into the machine**,
silently corrupting position. Nothing reports it and nothing catches it.

```c
uint8_t status = HAL_USART_INST.RXDATAH;
uint8_t b      = HAL_USART_INST.RXDATAL;
if (status & USART_FERR_bm) return;   // corrupted frame — not data
```

`RXDATAL` must still be read to clear the interrupt, so the cost is a test and a
branch. The existing read order (`RXDATAH` before `RXDATAL`) is already correct
and must stay — reading the low byte pops the FIFO and invalidates the status.

Beyond fixing the standing bug, this is what keeps §6's worst case small.

---

## 6. Worst case

**Steps until stop.** Node decides → asserts DE (5 µs guard) → drives low. The
Pico's readback mismatches within one byte time (~12 µs). The emit loop checks
twice per step, so it observes the fault within one step interval. At 60k steps/s
(16.7 µs/step) that is **~2–3 steps**; at lower feeds, fewer steps and more
wall-clock. Call it **under 5 steps end to end** — sub-10 µm on X at 160 steps/mm,
sub-5 µm on Z at 1200.

**What other nodes do with the corrupted bytes.** Contention leaves the line
indeterminate (§2.2), so receivers may resolve arbitrary bits. Each garbage byte
is then either:

| 9th bit | path | outcome |
|---|---|---|
| set | `frame_command_byte` | **fails CRC, discarded.** A stray partial command is wiped by the next `frame_stream_reset()`. Non-issue. |
| clear | stream byte, random step/dir bits | up to one step per engaged axis — **unless §5 lands**, which discards most on `FERR`. |

A 3-byte-time assertion is ~3 bytes, so the unmitigated worst case is a handful of
spurious steps per axis. Still microns. With the `FERR` check it approaches zero.

**This is why §5 is ordered first.** It is both the standing bug fix and the thing
that makes the alarm's blast radius negligible.

---

## 7. What this does not give you

**It carries exactly one bit: "someone is unhappy."** No identity, no reason, no
severity. The Pico must stop first and poll everyone afterwards to find out who
and why. For a panic signal that is the correct order — stop, then diagnose — but
it means the mechanism is a **stop, not a diagnosis**, and should never be
extended into one. Anything richer belongs in the status payload the follow-up
poll reads.

**False positives become machine stops.** Readback-compare demands a perfect echo
on every step. A leg of a few thousand steps with an unknown bus error rate
carries some probability of a spurious stop from ordinary noise.

That is the safe direction — an aborted probe costs a retry, a stopped job costs a
restart — but it could be annoying if the bus is noisier than expected. Start with
**fault on first mismatch** (a stop channel that debounces is not a stop channel)
and instrument it with a counter from day one, so "a node called for help" stays
distinguishable from "the cable is noisy." Same argument `limitBytesAsserted`
already makes for switch chatter ([homing.md](homing.md) §1.1), same resolution:
act immediately, record the lifetime total, decide thresholds from evidence.

**The path is exercised approximately never.** Unlike DMX, which hits its
detection path 44 times a second in every device, ours fires on a fault that may
not occur for months. Rarely-exercised safety paths rot silently — a refactor, a
shifted PIO offset, a changed SM allocation, and nothing tells you until the day
it matters. **A periodic self-test** — a node deliberately raising an alarm at a
safe moment, once at startup or before each job, with the Pico confirming it saw
it — converts a never-tested path into a routinely-tested one. Cheap, and worth
doing regardless of what else changes.

---

## 8. Precedent

The concept is not improvised. Three standards bear on it, and one argues against.

**DMX512-A (ANSI E1.11)** is RS-485 at 250 kbit/s, 8N2. Every packet is delimited
by a **break of ≥92 µs (~23 bit-times)** detected as a framing error — it is the
*primary* framing mechanism, load-bearing, in venue installations, 44 times a
second. It has no CRC and no acknowledgement at all; reliability comes purely from
repetition. It validates break *detection* thoroughly, and break *generation by
collision* not at all (§2.2).

**LIN** delimits frames with ≥13 dominant bits, detected the same way. Automotive,
tens of millions of vehicles.

**CAN is the closest analogue and the strongest endorsement.** CAN nodes signal
faults with an **error frame — six consecutive dominant bits** that deliberately
violate bit-stuffing, destroying the current frame for every node on the bus. That
is an unsolicited, any-node-any-time destructive stop signal: precisely what this
document proposes. A major fieldbus standard considered it important enough to
build into its core.

CAN can do this because its physical layer is **dominant/recessive** — dominant
actively drives, recessive is passive, so "someone is asserting" always beats
"nobody is asserting." That asymmetry also gives CAN non-destructive arbitration:
a transmitter that sends recessive and reads back dominant simply drops out, and
the winner's message survives intact.

**RS-485 has no such asymmetry.** Both drivers are symmetric push-pull, so a
conflict has no defined winner — which is exactly §2.2. This is the single
structural difference between our mechanism and CAN's, and it is the whole reason
readback-compare is needed instead of a clean dominant signal.

**RDM (ANSI E1.20) is the counter-evidence and should be recorded honestly.** RDM
added bidirectional communication to DMX over the same wire, using strict
time-division turnaround — never contention. And **it has no unsolicited-interrupt
mechanism at all**: a device that develops a fault sets a flag and *queues* the
message, and the controller discovers it by polling. A committee facing this exact
question, on this exact physical layer, chose polling over interruption.

Two readings. The cautious one: they were right, and the Pico should poll node
status between segments. The one this document takes: RDM's constraint is not ours
— a dropped DMX packet is a light doing the wrong thing in front of an audience,
whereas a corrupted stream byte costs microns (≈ zero once §5 lands), and nothing
in a lighting rig needs to stop within microseconds or damage itself. **The
deciding factor is latency:** polling between segments makes stop latency a
segment rather than a step, and the entire point is to report *before* position
error accumulates.

---

## 9. The alternative we would prefer, if a wire existed

A **shared open-drain ALARM line, wired-OR across all nodes.** Any node pulls it
low; the Pico reads a level.

Note what this actually is: an open-drain wired-OR line **is** CAN's
dominant/recessive asymmetry, in the simplest possible form. §8 identifies that
asymmetry as the one structural thing RS-485 lacks, and one conductor buys it.

Strictly better on every axis: no contention, no corrupted stream bytes, no false
positives from bus noise, no lost steps, and it works when the bus is idle *or*
when the node's UART is wedged — the one failure this design structurally cannot
cover, since raising the alarm requires the UART to work.

**So the real question is whether the harness has a spare conductor.** This should
be checked before building §3, because if it does, readback-compare becomes a
fallback rather than the plan. §5 is worth doing either way.

---

## 10. Deferred — the idle case

Readback-compare works only while the Pico is transmitting; with nothing sent
there is nothing to compare. A node needing to alarm while the bus is idle is
therefore **not covered by this document**.

The mechanism for it is the break detector that §2 rejected for streaming — and
in the idle case its premise *holds*, because the alarming node owns the line and
the low really is clean. A dedicated ~6-instruction state machine measuring
continuous low time would do it:

```
start:
    wait 0 pin 0        ; line went low
    set x, 30           ; threshold counter
count:
    jmp pin start       ; went high again → not a break, re-arm
    jmp x-- count [7]   ; still low → keep counting   (9 cycles/iteration)
    irq 0               ; stayed low throughout → BREAK
    wait 1 pin 0        ; wait for idle before re-arming
    jmp start
```

~34 bit-times at the values shown, against a longest-legal-low of 11 — real
margin, and it can claim an NVIC-routable flag (0–3) since it picks its own.
Resources are not a constraint: TX(6) + RX(9) + this(6) = 21 of 32 instructions
and 3 of 4 state machines on `pio0`, with PIO1 and PIO2 untouched.

The node's §3.2 primitive already drives low for ~3 byte times, so **it needs no
change when this is built** — the length was chosen with this in mind.

Also unresolved in the same family: during a **probe** the Pico is not
transmitting, so it cannot compare. Detection there falls back to framing errors
plus the vacuum's own echo check (§4.1). A third node alarming mid-probe is
therefore covered more weakly than during a normal stream. Known gap.

---

## 11. Bring-up order

Nothing here has run on hardware, and every figure is computed rather than
measured. §2.2 in particular reasons about how two SP3485s in contention resolve
at the receivers — exactly the sort of thing cheaper to measure than to argue
about.

1. **`FERR` check** (§5) — standalone, no dependencies, fixes a standing bug. Run
   normally for a while; if step accuracy improves, that is itself a finding.
2. **Scope the contention.** Drive the line low from one node while the Pico
   streams, and look at the pair. Confirm the ~0 V indeterminate prediction, and
   measure how many bytes are actually corrupted.
3. **Drain and compare** (§3.1) — verify it never faults across a long clean
   stream. **The false-positive rate over an idle hour is the number that decides
   whether §7 needs revisiting**, and it is currently unknown because nothing has
   ever looked.
4. **The node primitive** (§3.2) — confirm a deliberate assertion is detected
   every time, at both high and low step rates.
5. **First real caller** — the stepper stream gate. Trip a limit mid-job and
   confirm the machine stops instead of silently losing position.
6. **Periodic self-test** (§7) — before the mechanism is trusted, not after.

Steps 1 and 2 are worth doing even if the rest is abandoned.

---

## 12. Open questions

- **Is there a spare conductor?** (§9) Decides whether §3 is the right design at
  all. Check first.
- **Immediate or latched alarm on a refused step?** Immediate is more responsive;
  latched matches the glitch-tolerance argument in [homing.md](homing.md) §1.1 and
  avoids a chattering switch stopping every job. Undecided.
- **`LB_LAG_MAX` = 4** is reasoned, not measured. Step 3 of §11 should set it.
- **Does a node need to know its alarm was heard?** Currently it does not — it
  drives low and hopes. The Pico's stop is observable on the next poll, so there
  may be no need; but a node alarming into an idle bus (§10) gets no feedback at
  all.
- **Should the vacuum's echo check tolerate one mismatch?** §7 argues no. Revisit
  only with measured error rates.
- **Assertion length.** 3 byte-times is derived from "must exceed one frame" plus
  §10's future threshold, not measured. Step 2 of §11 should confirm it.
