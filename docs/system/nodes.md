# Nodes

**Checked against `4924837`.** To see what may have changed since:

```bash
git diff 4924837 -- src/node/ include/common.h
```

Commands, replies, status keys and code names are written as the
[notation](../README.md#notation) says.

A node is a small AVR on the RS485 bus that runs one axis or one peripheral
for the controller. It takes command frames addressed to it, answers them,
and, if it is a stepper, steps on the stream bytes for its slot. Each node
binary is one board, one node type and one node id, put together at build
time ([firmware build](firmware-build.md)).

This page is the node's side of the bus: how a node receives, what each type
does, and how a stepper moves, from the stream or on its own. The wire itself,
frames, stream bytes, slots and the status payload, is in [bus](bus.md).

## 1. Hardware

### Boards

| Board | Chip | Clock | Bus USART | Direction | LED | Console |
| --- | --- | --- | --- | --- | --- | --- |
| ATtiny3224 stepper | ATtiny3224 | 20 MHz internal | `USART1` | DE on PA3, software | PA5 | none |
| ATtiny3226 vacuum | ATtiny3226 | 16 MHz internal | `USART1` | DE on PA4, software | RGB: PA5 address, PA6 and PA7 activity | `Serial` (PB2/PB3) |
| DB32 stepper | AVR128DB32 | 24 MHz crystal | `USART2` | XDIR on PF3 | RGB on PF2/PF4/PF5 | `Serial1` (PC0/PC1) |
| DB32 vacuum | AVR128DB32 | 24 MHz crystal | `USART2` | XDIR on PF3 | one LED on PA3 | `Serial1` |
| DB32 knife | AVR128DB32 | 16 MHz crystal | `USART2` | XDIR on PF3 | RGB | `Serial1` |

Direction is how the transceiver's driver enable (DE) is switched. With
`RS485_USE_XDIR` the DB32's USART drives it in hardware; without, and always
on the ATtinys, the firmware raises DE before a reply and drops it after.

### Board files

Two headers describe a board, both found through the build's `-I` path:

* `board/<chip>/board.h`: what every type needs. The bus USART, its
  interrupt vector, the baud, DE, the LED and the console's serial port, as
  `HAL_` macros that expand to register accesses.
* `board/<chip>/<type>/<type>.h`: one type's pins on that board.

The type's code includes `"<type>/<type>.h"` and gets whichever board the
env chose. Nothing selects a board at run time.

### Each type's pins

* **Stepper:** step, dir and enable (DB32: PD4, PD5, PD6), the step-pulse
  timer TCB0, and the driver chip's pins. PD1 is the limit switch
  (`HAS_LIMIT_SWITCH`) or the Hall sensor (`HAS_HALL_INDEX`); an axis has one
  or the other, and a build with both fails. PD2 is the thermistor, or the
  laser's gate on the laser build (`NODE_HAS_LASER`).
* **Vacuum:** six servos, the pump's solid-state relay (SSR) and the probe
  switch (PA3 on the 3226, PA2 on the DB32).
* **Knife:** the oscillator's enable (PD1) and the blower's PWM (PD2).

## 2. I/O

### Controller to node

Command frames, and stream bytes for a node bound to a slot, over the 9-bit
bus at 921 600 baud (`RS485_BAUD`). See [bus](bus.md) §3 and §4.

### Node to controller

One reply per command frame addressed to this node, never to a broadcast.
Most replies that report state carry the status payload
(`buildNodeStatus()`, [bus](bus.md) §4). A vacuum built for probing also
answers a stream byte with one stream byte (§10).

### PC to node

A debug build (`NODE_DEBUG_CONSOLE`) listens on the console's serial port at
115 200 baud. A line of hex bytes, `[id] [cmd] [len] [payload…]`, goes in;
the console adds the CRC and runs it as if it came off the bus. It prints
`OK` and the reply without its CRC, `NAK unhandled`, or `ERR …`.
`node_console.py` is the PC side. The ATtiny3224 board names no console
port (`HAL_DEBUG_SERIAL`), so a console build for it fails.

### Node to hardware

Pins and timers on every type; the ADC on a Hall build; SPI to a TMC2660;
PWM for the blower; the Servo library for the vacuum's servos.

## 3. Lifecycle

### Boot

`setup()` drives DE low and the bus's TX pin idle, sets up the LED, then
calls the type's `node_setup()`. It starts the USART after that, enables
interrupts, and blinks the LED once per node id (none for 8 and up).

A node boots disabled, bound to no slot, with no datum and a counter at 0.
Nothing about the last power-on survives.

### The parts

The receive interrupt frames command bytes into a four-entry command queue
(`cmdQueue`) and, on a stepper, steps on stream bytes. `loop()` runs the
type's `node_loop()`, then takes one frame off the queue, checks its id and
CRC, and routes it. The core answers the generic commands; anything else goes
to the type through its hooks. §4 is the core, §5 the hooks, §6–11 the types.

### Make safe

Three things make a node safe:

* `CMD_DISABLE`: the type's `node_set_enabled(false)`, and the core clears
  `NODE_FLAG_ENABLED` and `NODE_FLAG_DATUM`.
* `CMD_MAKE_SAFE`: the same, then the type's `node_release()`, and answers
  with the status payload.
* Silence (`NODE_HAS_SILENCE_TIMEOUT`): `node_make_safe()`, as
  `CMD_MAKE_SAFE` with no reply, once after 1 s (`BUS_SILENCE_MS`) without a
  bus byte. The next byte re-arms it. It can't be combined with the console.

| Type | Disable | Release |
| --- | --- | --- |
| stepper | stop any leg, de-energise | drop the slot |
| vacuum | stop the pump | close every servo, drop the probe slot |
| knife | oscillator off, blower to 0 | nothing |

`CMD_DISABLE` and `CMD_ENABLE` are the only commands a node takes by
broadcast (`cmdAllowsBroadcast()`); the e-stop broadcasts the disable
([bus](bus.md) §6).

### Failures

* **Framing error:** counted (`busFerrCount`) and the byte dropped, since
  its 9th bit can't be trusted either; the frame it belonged to then fails
  its CRC. `NODE_IGNORE_FERR` keeps the byte instead, still counted.
* **Overflow:** a byte lost before the interrupt read it, counted
  (`busOvfCount`).
* **Bad CRC:** a frame for this node whose CRC fails is counted
  (`busCrcCount`) and dropped without a reply.
* **Queue full:** the frame is dropped at its end, and the next one still
  parses. The controller sees a missing reply.
* **Unknown command:** answered `CMD_NAK` with `NAK_UNSUPPORTED`, so a node
  that refuses can be told from one that isn't there.

`CMD_BUS_STATS` reads the three counters.

## 4. The core

### Receiving

The framer, `frame_command_byte()` in `rs485/frame.h`, is inlined into
whichever receive interrupt the build has. It counts every frame to its end,
`3 + len + 1` bytes, even one for another node or too long to store, so the
next frame starts in the right place. A frame is committed to the queue only
when it ends, is for this node or the broadcast id, and fits.

A stream byte (9th bit clear) resets the framer, so a frame cut short by a
stream byte is abandoned.

### Two dispatch paths

Commands and stream bytes take different paths:

* A **command** is queued by the interrupt and run from `loop()`, where a
  handler may take its time.
* A **stream byte** is acted on inside the interrupt, because a step can't
  wait for `loop()`.

That is why the receive interrupt belongs to the type. The stepper defines
its own, with the stream path inline (§7). Every other type compiles
`rs485/isr_generic.cpp`, which ignores stream bytes, except on a probe build
(§10). A stepper build that also compiled the generic one would define the
vector twice and fail to link.

### Two inputs

`routeCommand()` runs a frame through the generic commands, then the type's.
Two callers feed it:

* The bus, through `dispatchCommand()`: drops a broadcast that isn't
  allowed, answers an unhandled command with `NAK_UNSUPPORTED`, never
  answers a broadcast, and sends the reply.
* The console (`debug_console.cpp`): the same handlers, so the console can't
  drift from the bus. It prints the reply instead of sending it, and says
  `NAK unhandled` where the bus would send a NAK.

### Generic commands

The core answers these for every type:

| Command | Does | Reply |
| --- | --- | --- |
| `CMD_PING` | nothing | `CMD_PONG` |
| `CMD_GET_TYPE` | nothing | the type |
| `CMD_BUS_STATS` | nothing | the three error counters |
| `CMD_ENABLE` | `node_set_enabled(true)`, sets `ENABLED` | ack |
| `CMD_DISABLE` | `node_set_enabled(false)`, clears `ENABLED` and `DATUM` | ack |
| `CMD_MAKE_SAFE` | §3, Make safe | status |
| `CMD_DATUM_SET` | sets `DATUM` | status |
| `CMD_NODE_STATUS` | nothing | status |

The core owns the flags byte. Enabling doesn't set the datum: energising a
motor doesn't tell the node where the shaft is. Only `CMD_DATUM_SET` does,
and any disable clears it.

## 5. Hooks

`node_hooks.h` is the contract between the core and a type. The type
provides seven functions:

| Hook | Called | Does |
| --- | --- | --- |
| `node_type()` | `CMD_GET_TYPE`, the status | the `NODE_TYPE_*` constant |
| `node_setup()` | boot, before the USART | pins, timers, drivers |
| `node_loop()` | every `loop()` pass | non-blocking work |
| `node_set_enabled()` | enable, disable, make safe | what enabled means for the type |
| `node_release()` | make safe | drop the slot, park outputs |
| `node_handle_command()` | a non-generic command | handle it, or return false |
| `node_status()` | the status | the type's tail |

The hooks have no defaults, so a build that compiles no type fails to link.
Each type also checks `NODE_TYPE`, if the env sets it, against its own.

The core gives the types four functions back:

* `buildNodeStatus()`: the status payload, for a type's own replies.
* `node_make_safe()`: make safe without a reply.
* `node_set_flag()`, `node_flag()`: one bit of the flags byte. Set only from
  `loop()`: it reads, changes and writes a byte the core also writes, and an
  interrupt in between would lose a bit. A flag produced in an interrupt is
  kept in the type's own variable and copied over from `node_loop()`.
* `node_reply_nak()`: a refusal with a reason, `[id][CMD_NAK][2][cmd]
  [reason]`, for a type that can say why. The handler returns true, since a
  reply was staged.

## 6. Stepper

A stepper moves one motor, two ways: on stream bytes from the controller
(§7), or on its own, running a leg it was told to run (§8). Both share:

* **The counter** (`absolutePosition`): one count of steps taken, whichever
  path took them. It is what the status reports as the position.
* **DIR** and the last level driven on it (`currentDir`).
* **The pulse** (TCB0): every step raises STEP and starts TCB0 as a
  one-shot that drops it 3 µs later, so a step has the same shape from either
  path.

One path owns the axis at a time. While a leg runs (`legActive`), the stream
path returns before it reads anything from the byte.

### Driver chips

Chosen by flag, one per build. `drivers_init()` runs in `node_setup()`
before the first disable.

* `DM542`: enable low. Nothing to set up.
* `DRV8825`: enable low, and the microstep pins set from
  `DRV_MICROSTEPPING`.
* `TMC_2660`: set up over SPI with `TMC_CURRENT` and `TMC_MICROSTEPPING`.
  Enable and disable also switch the chip's output stage (`toff`).

### The laser

On the laser build, `CMD_LASER` switches the laser's gate on PD2. It boots
off. Make safe leaves it as commanded: it is a crosshair, not a tool.

## 7. Stream motion

The stream path is the end of the stepper's receive interrupt, for a byte
with the 9th bit clear.

### The slot

`CMD_ENGAGE` binds the node to one of the stream byte's four slots, 0–3, or
to none (`0xFF`). The slot sets two masks: the step bit and the dir bit this
node reads. Unbound, both are 0 and every stream byte is ignored, so the
counter freezes. The ack is the status payload, taken after the bind, so the
controller sees the slot, the position and whether the motor is energised in
one reply. Binding and energising are separate: a parked head is energised to
hold its height but bound to nothing ([bus](bus.md) §3).

### A step

For a byte that passes the slot and the limit gate:

1. If the byte's step bit for this slot is clear, nothing happens. DIR
   moves only on a byte that steps this axis, because every byte carries a
   dir bit for every slot.
2. If the dir bit differs from `currentDir`, DIR is driven and the
   interrupt waits 5 µs, the DM542's setup time.
3. STEP goes high, the counter moves one, and TCB0 starts.

### The limit gate

On a build with a switch, the stream refuses every step, both ways, while
the switch reads pressed. A machine that has run onto a hard stop stops,
whatever sent it there. Driving off the switch is a retract (§8), not a
stream move.

The refusal is immediate; a latch decides whether it outlasts the press. The
gate counts stream bytes seen while pressed. A press that lasts 500 ms of
stream bytes (`LIMIT_LATCH_MS`) latches (`limitLatched`), and the gate stays
shut after the switch releases. A shorter press was a glitch: the gate opens
on release and the axis has lost the steps it refused.

The time base is bytes, not a timer: a stream byte is 11 bit times at the bus
baud, so it is a fixed tick with no timer read in the interrupt. The count
since boot (`limitBytesAsserted`) is never reset, so a chattering switch shows
even when no press latched.

Only a retract that ends clear of the switch clears the latch. `node_loop()`
publishes the pin or the latch as `NODE_FLAG_LIMIT`.

The switch is wired to ground against the pull-up, so a cut wire reads as
pressed. `LIMIT_ACTIVE_HIGH` turns the reading round for a switch wired the
other way; wrong, the gate runs backwards.

## 8. Node-driven motion

A leg is a move the node times itself, on TCA0, from a ramp, a step budget
and a stop condition the controller sends. The node measures and reports; it
doesn't know legs come in pairs or what a datum is
([homing](../machine/homing.md)). Only a build with a switch or a Hall
sensor (`HAS_HOMING`) has legs; any other NAKs them.

### Modes

| Mode | Started by | Ends on | Then |
| --- | --- | --- | --- |
| seek | `CMD_HOME_LEG`, switch clear | the switch, debounced, or the budget | latches if it hit the switch |
| retract | `CMD_HOME_LEG`, switch pressed | the budget | clears the latch if the switch is clear |
| sweep | `CMD_HOME_LEG`, Hall build | the index found (§9), or the budget | resolves the index |
| park | `CMD_PARK_LEG` | the target, or the switch | latches if it hit the switch |

On a switch build, `CMD_HOME_LEG` doesn't name its mode. The node reads the
pin once, when the command arrives: clear is a seek, pressed is a retract.
The command's intent bit is checked against that read, and a mismatch is
refused with `NAK_INTENT_MISMATCH` before anything moves: a seek's budget is
sized as a runaway cap, and a retract would run all of it. Deciding from the
pin, not the latch, means a node that boots on its switch retracts on its
first command.

A seek believes its switch after three pressed samples in a row
(`LEG_LIMIT_SAMPLES`), one per step. A single transient no longer stops it.

### Arming

`legArm()` refuses rather than clamps, since each refusal is a mistake on the
controller's side:

* `NAK_BUSY`: a leg is already running.
* `NAK_BAD_ARG`: a zero interval or budget, a floor slower than the start,
  or a start too long for the 16-bit timer. A frame too short gets the same.
* `NAK_NO_DATUM`, park only: the datum is clear, so the counter isn't a
  position.
* `NAK_INTENT_MISMATCH`, park only: the switch is pressed or latched.

The intervals arrive in microseconds and are turned into TCA0 ticks with the
board's clock, so the controller never needs it. DIR is set once, before the
first step, so the pulser never waits for it. The counter at the arm is kept
for the span.

The arm takes TCA0 out of the split mode DxCore leaves it in for PWM:
CTRLA is cleared first, since a write to CTRLD while the timer runs is
dropped. No stepper build uses PWM, so the timer isn't given back.

A park computes its distance from the counter. Already at its target, it
arms nothing and acks with `NODE_FLAG_LEG` clear.

### The pulser

One step per TCA0 overflow. Stop conditions are checked before the step, so
a leg never steps past what ended it. The interval shortens by a fixed amount
each step until it reaches the floor: a linear ramp, gentler than constant
acceleration at the start, where a motor stalls. A park ramps back down over
the steps it took to ramp up, so a short park is a triangle.

### Finishing

The interrupt can't finish a leg: the flags may only be written from
`loop()`. It stops the timer and sets `legFinished`, and the next `legLoop()`:

1. Closes the span, how far the leg moved, signed, failed or not
   (`legSpanSteps()`, in the status).
2. Clears the latch after a retract that ends clear of the switch, or sets
   it after a seek or park that hit it.
3. Starts the Hall resolve after a sweep (§9).
4. Publishes `NODE_FLAG_LEG`, which stays set until the leg has an answer,
   including the resolve.

### Stopping a leg

* `CMD_LEG_ABORT` stops the pulser and nothing else. The motor stays
  energised, so the counter and the datum stay good.
* Disable stops the pulser and de-energises, so a broadcast e-stop stops a
  leg too.

## 9. The Hall index

A rotary axis has no switch. A magnet on the output side passes a fixed Hall
sensor once a revolution, and the dip in its analog output marks the index.
A dip's centre is known only after passing it, so a sweep runs through the
whole dip, keeps its samples, and works out the centre afterwards. The
measurements behind the constants are in
[rotary_a_axis.md](../rotary_a_axis.md).

### Sampling

`hallIndexSample()` runs in the pulser after each step, so each sample
belongs to the position just reached. It reads the free-running ADC and
keeps the highest reading as the baseline, away from the magnet. A dip starts
400 counts below the baseline and ends 300 below. Each crossing's position is
kept; only the last one is captured, in a 400-sample window.

The sweep stops when the last of three crossings (`HALL_CROSSINGS`) has
passed. Two prove the dip
repeats, so it is the index and not a transient; three prove it twice, so
two laps that disagree show a slip.

Every step-domain number is measured by the sweep: the lap length, the dip's
width. Gearing differs between heads, and constants carried from one head to
another were wrong on the second.

### Resolving

The centre is found by correlating the window with its mirror image, which
grows with the square of the window. Run in one go it would keep `loop()`, and so the bus, busy
for about 170 ms, and the controller would fail the leg on missed polls. So
`hallIndexResolveBegin()` settles the cheap cases and
`hallIndexResolveStep()` does one bounded slice per `loop()` pass.
`NODE_FLAG_LEG` stays set until it finishes.

### Answer

The status tail gains the index position, a cause (`ROTARY_IDX_*`: OK,
NOTFOUND, DEGENERATE, OVERFLOW, SLIP), the live and baseline readings, the
measured steps per revolution, and the crossings completed. Zero crossings
means the magnet was never seen; fewer than three, that the budget ran out.

## 10. Vacuum

The vacuum node runs the bed's hold-down pump and six servos that open and
close its zones, and on a probe build reads the tool-height probe switch.

### The pump

`CMD_SSR_SET`, or enable, starts the pump on its solid-state relay with a
soft start: the duty ramps from 40 % to 100 % over 5 s, to limit inrush.
An SSR switches only at a mains zero crossing, so duty is whole 10 ms cycles
out of each window of ten, spread evenly. `node_loop()` advances it on
`millis()`, not locked to mains. Disable stops it.

### Servos

`CMD_SERVO_SET` sets one servo (1–6) or all (0) to an angle, 0–180°. They
boot at 0°, and release closes them all.

### The probe switch

`CMD_SWITCH_GET` reads the switch: 1 open, 0 closed. It is normally closed,
to ground. On the 3226 the chip's pull-up holds the pin high when the switch
opens, so a cut wire reads open. The DB32 sets the pin as a plain input, with
no pull-up from the chip; how a cut wire reads there is not known.

A probe build (`NODE_HAS_PROBE_REPLY`) also takes `CMD_ENGAGE`, which binds a
slot to poll, not to step. A stream byte with this slot's step bit set asks
for the switch, and the node answers inside the receive interrupt
(`probe_stream_byte()`) with one stream byte: the slot's dir bit set if the
switch reads closed, every step bit clear. Answering from `loop()` would put
the pump's state machine in the reply's path. A byte already waiting when the
answer is ready means the node is late, and it stays silent rather than
collide. A lost or garbled answer reads as open, which stops the probe
([bus](bus.md) §3, Probing).

The stepper's `CMD_ENGAGE` and this one share an opcode and a payload but
not a meaning, so each type has its own handler.

### LEDs

On the 3226 board, red is lit while the pump runs and green while every servo
is at 0°. The DB32 board has one LED, owned by the address blink, so the type
lights neither.

## 11. Knife

The knife node drives the drag knife's oscillator, on or off
(`CMD_KNIFE_OSC`), and the chip blower at a duty of 0–100 %
(`CMD_KNIFE_BLOWER`). Both outputs hold their level, so `node_loop()` does
nothing. Enable does nothing either; disable turns both off. It has no slot.

## 12. Who writes what

| Value | Writer | Readers |
| --- | --- | --- |
| `cmdQueue`, `cmdHead` | the receive interrupt (the framer) | `loop()` |
| `cmdTail` | `loop()` | the framer |
| `busFerrCount`, `busOvfCount` | the receive interrupt | `CMD_BUS_STATS` |
| `busCrcCount` | `loop()` | `CMD_BUS_STATS` |
| `busHeard` | the receive interrupt | `loop()`, silence |
| the flags byte (`g_nodeFlags`) | the core; types through `node_set_flag()`, from `loop()` | the status |
| `absolutePosition` | the stream path, the pulser | the status, `legArm()`, the span |
| `currentDir` | the stream path, `legArm()` | the stream path |
| slot, `stepBitMask`, `dirBitMask` | `CMD_ENGAGE`, release | the stream path, the status |
| `limitLatched` | the stream path sets; `legFinish()` sets and clears | the stream path, park, `node_loop()` |
| `limitBytesAsserted`, `limitRunBase` | the stream path; `legFinish()` resets the base | the stream path |
| the leg (`leg`, `legActive`) | `legArm()`, then the pulser | the stream path, `legLoop()` |
| `legHitLimit`, `legFinished` | the pulser | `legFinish()` |
| the span | `legFinish()` | the status |
| the Hall window and answer | the pulser, then the resolve in `loop()` | the status |
| `probeSlot`, `probeStepMask`, `probeDirMask` | the vacuum's `CMD_ENGAGE`, release | the probe answer, the status |
| the pump state | `CMD_SSR_SET`, enable, disable, `node_loop()` | the status |
| the servo angles | `CMD_SERVO_SET`, release | the status |
| the oscillator, the blower duty | the knife's commands, disable | the status |

## 13. Decisions

* **Mandatory hooks.** A missing type fails at link. Weak defaults would
  build a node that answers ping and does nothing.
* **The stepper owns its receive interrupt.** The stream path inlines into
  it with no call. A shared interrupt calling a type's stream handler would
  spill registers on every byte, at up to one byte every 12 µs.
* **The slot is separate from enable.** A parked head holds its height
  without stepping. One flag for both would drop the head or step it.
* **A leg's mode comes from the pin.** One read at arm time, checked against
  the intent. Taking the mode from the controller alone would run a
  budget under the wrong rule when the two disagree; taking it from the
  latch would make a node that boots on its switch seek first.
* **The limit gate counts bytes.** Fixed by the baud, so no timer is read in
  the interrupt. Counting steps would measure distance: a slow axis would
  take far longer to latch than a fast one.
* **The gate refuses at once and latches later.** Refusing only after the
  latch would let an axis run 500 ms into its stop.
* **The Hall index is resolved in slices.** In the interrupt it can't fit;
  in one `loop()` pass it silences the node for 170 ms.
* **The probe answers from the interrupt.** From `loop()`, the pump's
  state machine would sit in the reply's path.

## 14. Code

| File | Holds |
| --- | --- |
| `include/common.h` | the wire contract shared with the controller: commands, NAK reasons, flags, node types, the broadcast allowlist |
| `src/node/main.cpp` | boot, `loop()`, silence |
| `src/node/dispatch.cpp` | generic commands, routing, NAK, the flags, make safe |
| `src/node/node_hooks.h` | the hook contract |
| `src/node/protocol.h` | the command queue's shape |
| `src/node/debug_console.cpp` | the console |
| `src/node/rs485/frame.h` | the framer and the error checks |
| `src/node/rs485/isr_generic.cpp` | the receive interrupt for every type but the stepper |
| `src/node/rs485/rs485.*` | the queue, the counters, sending a frame |
| `src/node/board/` | board and per-type pin headers |
| `src/node/types/stepper/stepper.cpp` | the stepper's hooks and commands, its receive interrupt, the limit gate |
| `src/node/types/stepper/stepper_state.h` | the counter, DIR and the latch, shared with the pulser |
| `src/node/types/stepper/leg.*` | arming, the pulser, finishing |
| `src/node/types/stepper/hall_index.*` | sampling and resolving the index |
| `src/node/types/stepper/drivers.cpp` | the driver chips |
| `src/node/types/vacuum/vacuum.cpp` | the pump, the servos, the switch, the probe engage |
| `src/node/types/vacuum/probe_slot.h` | the probe answer |
| `src/node/types/knife/knife.cpp` | the oscillator and the blower |
