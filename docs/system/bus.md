# Bus

**Checked against `dd7d63b`.** To see what may have changed since:

```bash
git diff dd7d63b -- include/common.h src/rp2350/core1/bus/ src/rp2350/core1/rpc_server.cpp src/rp2350/core1/core1.cpp src/rp2350/core0/ops/bus.cpp src/rp2350/core0/ops/slot_map.cpp src/rp2350/core0/ops/axes_map.cpp src/rp2350/core0/ops/position.cpp src/node/rs485/ src/node/main.cpp src/node/dispatch.cpp
```

Commands, replies and code names are written as the
[notation](../README.md#notation) says.

One RS485 pair joins the controller to every node. Over it the controller
sends two kinds of traffic: stream bytes, which step the motors, and command
frames, which talk to one node and wait for its answer. This page is that
wire, from the hardware up: how a word is sent, the two kinds of traffic, and
how the controller knows the bus is safe.

## 1. Hardware

* **Half-duplex RS485:** one differential pair shared by the controller and
  every node. One talker at a time; whoever talks drives the line, everyone
  else listens.
* **Half-duplex transceivers:** one per board. A board enables its driver only
  while it sends, and releases the line after.
* **USARTs in 9-bit mode:**
  * **The nodes** (AVR) have it natively: the USART sends and receives a ninth
    data bit.
  * **The controller** (RP2350) has no 9-bit USART, so two PIO state machines
    on `pio0` emulate one: one sends and drives the transceiver's enable pin,
    one receives. Pins 4 (TX), 5 (RX), 6 (enable).

## 2. Data layer

* **9-bit words at 921600 baud** (`RS485_BAUD`): a start bit, nine data bits,
  a stop bit. The ninth bit says which kind of traffic the word is (§3, §4).
* **Theoretical bandwidth:** 11 bit times a word, 83,800 words a second,
  11.9 µs each.
* **Theoretical speed, as the controller sends:** its PIO spends 12 bit times
  a word (the extra one enables the transceiver before the start bit), so
  76,800 words a second, 13 µs each. Back to back, that is the fastest a stream
  byte can follow another: one step per slot every 13 µs at most. What that
  is in mm/s depends on each axis's steps per mm.
* **Other limits:**
  * **Half duplex:** while a node answers, the controller cannot send, so a
    command frame and its answer pause the stream.
  * **One answer at a time:** the controller waits up to 20 ms
    (`RESPONSE_TIMEOUT_MS`) for each answer before the next frame.
  * **The node's receive buffer:** four frames (`MAX_COMMANDS`) of up to 32
    bytes (`MAX_PACKET_LEN`); a longer frame is counted and dropped.

## 3. Stream bytes

A stream byte is a word with the ninth bit 0. Its eight data bits are four
2-bit slots, a step and a direction each:

```
 9th   7     6     5     4     3     2     1     0
[ 0 ][dir3][stp3][dir2][stp2][dir1][stp1][dir0][stp0]
       slot 3      slot 2      slot 1      slot 0
```

A stream byte names no node: a node acts on the slot it holds (Slots, below)
and ignores the rest. A node that holds no slot ignores the byte.

An empty stream byte, all zeros, asks nothing of anyone. It is the resync:
every stream byte abandons a half-received command frame on every node (§4).

What a slot's bits mean depends on the node that holds it. A stepper reads
them as a step and a direction, and never answers. A node that reports, like
the probe's vacuum, reads its step bit as a question and answers with a stream
byte of its own. So a stream byte is one of two kinds.

### Without reply

Every slot is held by a stepper, or empty. Whichever stepper holds slot n
steps once when its step bit is set, in the direction its direction bit gives.
During motion Core 1 sends one byte per step interval, back to back.

* **A stepper moves its direction pin only on a byte that steps it,** and
  ignores the stream while its own leg runs or while its limit switch refuses
  (the `nodes` page).
* **Unacknowledged:** a stepper that misses a byte misses a step, and nothing
  on the wire says so. It shows only as a position the node reports that
  differs from the controller's.

### With reply

The step bit of a reporting node's slot is set. That node answers with one
stream byte: every step bit clear, so it moves nothing, and its own slot's
direction bit carrying the answer.

* **Lockstep:** the controller sends nothing more until the answer arrives or
  its deadline passes. Silence past the deadline is a failure, not a no.
* **Fail-safe encoding:** the answer's bit is set only for the state that lets
  motion go on (for the probe, the switch read closed). A lost or corrupt byte
  reads as the safe answer.
* **No CRC:** the answer is one unchecked word. A decision that must not rest
  on it is confirmed by a command frame (the probe's contact is).
* **Answered from the receive interrupt,** and dropped rather than sent late:
  a late answer would collide with whatever the controller sent next.
* **One reply per byte.** Every node answers on the same wire, so only one
  slot in a byte may ask; two answers would collide.
* **Costs at least half the bandwidth.** Each question takes two words on the
  wire, question and answer, plus the node's interrupt latency and two
  transceiver turnarounds; the probe measures about 40 µs a poll against 13 µs
  for a byte without reply.

### Mixes

| Mix | Each byte | Rate | Used by |
| --- | --- | --- | --- |
| Pure, without reply | steps only | the byte rate (§2) | jobs, jogs, the probe's retract |
| Pure reply | asks one node, steps nothing | one round trip per byte | the probe's contact confirm |
| Hybrid | steps, and asks one node | one round trip per byte | the probe's descent, which steps Z and asks the vacuum in the same byte |

A hybrid need not ask on every byte: the probe's descent asks every few steps
(its poll divider) and sends the steps between without reply, so it runs
close to the pure rate while still stopping within a few steps of the switch.

A bed-level node, when it joins the bus, will report the same way as the
probe's vacuum: a slot, a question on its step bit, one answer byte.

**Limits:**

* **Four slots** (`MOTION_SLOTS`): at most four nodes, stepping or reporting,
  per byte.
* **One reply per byte,** and lockstep while one is asked.
* **One step per byte per slot,** so a slot's step rate is at most the byte
  rate (§2), and at most the round-trip rate on a byte that asks.

### Slots

The machine has more motors than slots: two heads are six steppers. Which
node listens on which slot is set at run time, by command frames to one node
at a time (§4):

* **Engage** (`CMD_ENGAGE`) carries a slot, 0–3, or 0xFF (`SLOT_NONE`) to
  leave it. The node answers with its status (§4), so one transaction shows it
  bound, its position and whether it is energised.
* **Engage and energise are separate.** A node must be both to move. A parked
  head stays energised, holding its height, but disengaged.
* **Make-safe** (`CMD_MAKE_SAFE`) de-energises, clears the node's datum, and
  leaves the slot, in one transaction; its status is sampled after. Released
  means no slot in the status and not energised.

A node boots disengaged.

**The slot table.** Core 0 keeps which node holds each slot (`slotNode`), and
which slots are fenced (`slotFenced`). Only the slot map writes it.

**The slot map command.** `slot_map <n0> <n1> <n2> <n3>` takes one bus id per
slot, in slot order: `<n0>` is the node to bind to slot 0, and so on. `-` or
`0` leaves a slot empty. Any node type, no config needed.

```
slot_map 1 2 3 4     node 1 on slot 0, node 2 on slot 1, node 3 on slot 2, node 4 on slot 3
slot_map - - 3 7     only slots 2 and 3: node 3 and node 7
slot_map             read back the binding: → slot_map - - 3 7
```

The read-back prints one entry per slot: `-` for empty, `n` for node n bound,
`!n` for a slot fenced on node n (the fence, below).

**Applying a slot map.** `slot_map` with ids sets the slot request
(`slotMapApply()`) and applies it. Applying is not a diff: it re-sends every
engage, so a node that reset and silently lost its slot is bound again.

1. **A fenced slot the request names** first gets make-safe to its node.
2. **Every bound, unfenced slot** is parked: engage 0xFF to its node.
3. **Every requested slot** is engaged, in order. Each slot adopts its axis
   position and energised bit from the engage's answer. A stepper whose
   counter moved while it was parked loses its origin.
4. **A failed engage stops the apply:** later slots stay unbound.

The command answers `ok` even when the result is short of the request: the
slot-unmapped fault (`F_UNMAPPED`) says so, held while the binding differs
from the request (`slotMapComplete()`). With nothing requested since the last
reset it is clear. `unalarm` retries the request once (`slotMapRetry()`).

**The fence.** A slot is freed only on its node's word. A park that is not
confirmed, or an engage with no answer, fences the slot (`slotFence()`): it
keeps the node's id, the node's origin is voided, and the slot's axis reads
unhomed at 0. A NAK is a confirmed refusal and fences nothing.

A fenced slot takes no engage, satisfies a request of `-` only, and reads back
as `!n` (`slot_map 1 2 !3 4`). It clears only on a make-safe whose answer
shows the node released, or an engage that shows it in another slot. A
request that names a fenced slot whose make-safe still fails answers
`err fenced <s0> <s1> <s2> <s3>`, and the rest of the map applies.

`makesafe <id>`, confirmed, unbinds the node's slots and drops it from the
slot and axes requests, so no fault follows; unconfirmed, it fences them.

### Axes

Axis k is slot k: X, Y, Z, A on slots 0–3. `axes_map <x> <y> <z> <a>` takes
the bus id of the node to drive each axis, in that order; `-` or `0` for no
node:

```
axes_map 1 2 5 6     X is node 1, Y node 2, Z node 5, A node 6
axes_map             read back the request: → axes_map 1 2 ?5 6
```

The read-back prints `?n` for an axis still pending on node n (below).

It stores which node each axis should be (the axes request, `axesReqAt()`),
then applies the same ids as a slot map (`axesMapApply()`):

1. Every named axis starts pending.
2. Each named node gets a status read (`CMD_NODE_STATUS`), or make-safe if it
   holds a fenced slot. A confirmed stepper clears its pending.
3. A confirmed node of another type refuses the whole map,
   `err node <id> not_stepper`, and nothing changes. The config's default map
   keeps it pending instead, so a wrong config boots into `F_UNMAPPED`.
4. A node that does not answer stays pending, `err node <id> timeout`.
5. The ids are committed as the slot request; a pending axis's slot is parked,
   not engaged, so the request stays unmet until `unalarm` re-checks
   (`axesMapRetry()`).

The axes map needs a config: every id must be an axis node the config marks
present. `select`
applies a head's map.

An axis is bound while its slot holds the node the axes request names and is
not fenced. Position, homed, latched and energised read 0 for an unbound axis.

### Probing

The probe switch is wired to the vacuum node, not to Z, so the probe closes
its loop over the stream. A probe binds the slot map `- - <z> <vac>`: Z on
slot 2 and the vacuum (`NODE_HAS_PROBE_REPLY`) on slot 3. The vacuum's step
bit asks for the switch, and its answer's direction bit is set only while the
switch reads closed (With reply, above). The descent is a hybrid, the contact
confirm a pure reply, the retract without reply.

The probe's exit re-applies the axes request, which parks the vacuum. The
probe leg that sends these is the `probe` page's.

## 4. Command frames

A command frame is a run of words with the ninth bit 1:

```
 9th
[ 1 ][id]  [ 1 ][cmd]  [ 1 ][len]  [ 1 ][payload…]  [ 1 ][crc8]
```

* **crc8** covers everything before it (`crc8()`, in `include/common.h`).
* **Before every frame** the sender sends an empty stream byte
  (`busQuiesce()` on the controller), so a node's parser that lost count
  starts clean.
* **A node counts every frame to its end,** `3 + len + 1` bytes, even one for
  another id or too long to keep, so the next frame still parses.
* **A byte with a framing error** is dropped and resets the parser; its frame
  then fails its CRC.

### Addressing

**Node ids and the bus limit.** A node's id is set in its build (`NODE_ID`).
The controller addresses ids 1 to 8 (`BUS_ADDR_MAX`): its masks of nodes are
16 bits wide, and the sweeps walk every id, each absent one costing a 20 ms
timeout.

**Broadcast.** A frame to 0xFF (`BUS_ADDR_BROADCAST`) is acted on by every node
and answered by none: replies would collide. Only an allowlisted command may
be broadcast (`cmdAllowsBroadcast()`), checked at both ends: disable and
enable. After a broadcast disable Core 1 clears every node's energised bit;
after a broadcast enable it changes nothing, since believing a node armed that
never heard is the dangerous direction.

### Generic and type-specific commands

| Range | Honoured by | Commands |
| --- | --- | --- |
| 0x01–0x1F | every node, in the node's core | ping and pong, enable, disable, type, NAK, receive errors (`CMD_BUS_STATS`), make-safe |
| 0x10–0x14 | the vacuum and knife types | servos, SSR, knife oscillator and blower, the probe switch |
| 0x20 up | one type each, so values overlap between types | the stepper's engage, status, laser, datum, legs |

Only generic commands may be broadcast: a type-specific value means different
things to different types. The full list is in `include/common.h`; what each
does on the node is the `nodes` page's.

### Answers

The controller sends a frame and waits up to 20 ms for one from that id
(`receivePacket()`). The answer is one of:

| Answer | Means | RPC result |
| --- | --- | --- |
| the same command, its CRC good | done; the payload is its reply | `RPC_OK` |
| `CMD_NAK`, with the command and a reason | the node is there and refused | `RPC_NAK` |
| nothing | absent, or the frame was lost | `RPC_TIMEOUT` |

`CMD_PING` is the one command answered by another, `CMD_PONG`. A node answers
an unknown command with a NAK (`NAK_UNSUPPORTED`).

### The status

Every command that reports state answers with one status payload
(`buildNodeStatus()` on the node, `nodeStatusDecode()` on the controller):

```
[type][flags][type's tail…]
stepper tail: [pos int32][slot]…
vacuum tail:  [servos][ssr][slot, probe builds only]
```

The flags are energised (`NODE_FLAG_ENABLED`), the datum witness
(`NODE_FLAG_DATUM`), on its switch (`NODE_FLAG_LIMIT`) and leg running
(`NODE_FLAG_LEG`). Core 1 folds the energised flag of every confirmed status
into which nodes are energised (`nodeEnabled`, [cores](cores.md) §5).

## 5. I/O

**Core 1 to the bus:** Core 1 is the only part of the controller that touches
the wire ([cores](cores.md)).

* **Stream bytes** from the step emitters and the debug step burst.
* **Command frames** for each RPC request: a node command, or the probe leg's
  polls.
* **The keep-alive** and **the e-stop sweep** (§6).

**The bus to Core 1:** the answer to a command frame, and the probe vacuum's
answer to a stream byte.

**Core 0 to the bus,** through the RPC: the slot map, the axes map, the boot
sweep, and every node command. Core 0 owns which node is in which slot; Core 1
only sends what it is asked.

**The host to Core 0:** `slot_map`, `axes_map`, `makesafe`, `bus_exclude`,
`bus_enable`, `pingnode`, `nodestat`, `busstat`
([commands](../reference/commands.md), Bus and nodes).

## 6. Lifecycle

### Boot

1. **A node** boots disengaged.
2. **Core 1** starts its port, and after Core 0's first wipe sends an empty
   stream byte so every node's parser starts clean ([cores](cores.md) §3).
3. **Core 0** sweeps the bus (`busSweep()`): make-safe to every id from 1 to 8.
   A node the config names, or one the controller had touched, that does not
   confirm is mute (`busMute()`). A mute node not excluded holds the bus fault
   (`F_BUS`); `bus_exclude` runs without it.
4. **Core 0** applies the config's default map through the axes map (§3).
   Without a config nothing is requested and nothing is bound.

A soft reset forgets the slot and axes requests but keeps the slot table, its
bindings and its fences; only power-on clears them. The boot sweep then frees
every slot whose node confirms.

### E-stop

1. **Core 0**, at `stop`, fences every bound slot (`slotFenceAllBound()`):
   the sweep's answers stay on Core 1, so Core 0 cannot know which nodes let
   go.
2. **Core 1** sends one broadcast disable, so every node starts stopping at
   once, then make-safe to each id in turn (`busStopAll()`), clearing a node's
   energised bit only on its confirmation. Then it sets the e-stop swept
   (`estopSwept`) ([cores](cores.md) §3).
3. **Core 0** clears the fenced slots' positions (`slotFencedPosClear()`) once
   Core 1 has stopped writing them.

The sweep covers every id, not just the slots: a vacuum or knife holds no
slot, and must stop too.

### Silence

A node built with the silence timeout (`NODE_HAS_SILENCE_TIMEOUT`) makes
itself safe after 1 s without a word (`BUS_SILENCE_MS`). Any word that passes
the framing check feeds it, stream or command, to any id. No build enables it
today. Core 1 keeps an idle bus alive for it (`busKeepalive()`): an empty
stream byte after 333 ms without sending (`BUS_KEEPALIVE_MS`). During motion
every step sends a byte. Core 1 sends nothing while parked for flash, so a
config write longer than 1 s would leave such a node safe.

A node's own release is invisible to the controller: the map still says it is
bound. The next engage, or a status read, finds it.

## 7. Who writes what

| Value | In code | Written by | Read by |
| --- | --- | --- | --- |
| A node's slot | `slot` (stepper), `probeSlot` (vacuum) | the node, on engage, make-safe or silence | the node's receive interrupt |
| The slot table | `slotNode`, `slotFenced` | Core 0: the slot map, make-safe, the e-stop fence | Core 0: the axes, the faults, `slot_map` |
| The slot request | `slotReq` | `slot_map`, `axes_map`, `makesafe`; the soft reset forgets it | `slotMapComplete()`, the retry |
| The axes request, pending axes | `axesReqAt()`, `axesReqPending()` | `axes_map`, `select`, the default map, `makesafe` | the axes map, `axes_map` |
| Which nodes are energised | `nodeEnabled` | Core 1, from confirmed answers ([cores](cores.md)) | Core 0 |
| Mute, excluded | `busMute()`, `busExcluded()` | the boot sweep; `bus_exclude` | the bus fault, the RPC's refusal |
| Last sent | `takeSent()` | every write to the port | the keep-alive |
| Receive errors | `busFerrCount`, `busOvfCount`, `busCrcCount` | the node | `busstat` (`CMD_BUS_STATS`) |

## 8. Decisions

* **One wire, two kinds of traffic by the ninth bit.** Steps need no address
  and no answer, so a stream byte is one word; commands need both. The ninth
  bit sorts them in the node's receive interrupt with no parsing.
* **A reporting node answers in the stream, not by command frame.** A frame
  and its answer cost a quiesce byte, at least five words each way and a CRC;
  a stream question and answer cost two words, so the probe can stop within a
  few steps of the switch. The price is lockstep, one reply per byte, and no
  CRC, so a decision is confirmed by a frame.
* **Slots set at run time, not from the bus id.** A fixed id-to-slot rule
  allowed only ids 1–4 and one head. Two heads need six motors on four slots,
  with the head switch rebinding Z and A.
* **Engage one node at a time, never broadcast.** A stream byte names no node,
  so a missed engage would move the wrong motor with nothing to catch it.
  Each engage is addressed and answered.
* **Re-send every engage, not a diff.** A diff of an unchanged map sent
  nothing, so a node that had reset stayed unbound while the map said bound,
  and motion streamed into a slot nobody heard (seen on the bench).
* **Freed only by confirmation: the fence.** Not on time or silence: a node
  can miss whole frames while words still feed its timer.
* **No rollback on a partial apply.** The request stays, the fault says it is
  unmet, and a retry re-sends the same engages; re-engaging a node in its own
  slot changes nothing.
* **A NAK, not silence, for a refusal.** A refusal dropped like a bad frame
  looked the same as an absent node, and cost a timeout.
* **0xFF for broadcast, deny by default.** Zero is what a cleared buffer
  holds, so a bug would broadcast by accident; and a command no one cleared
  for broadcast cannot take effect through one.
* **Broadcast disable before the serial sweep.** Every node starts stopping at
  once; the sweep behind it is what confirms, and gates ALARM.
* **The map lives on Core 0.** It is the host's word, so it sits with the
  host's side; Core 1 sends single engages and knows nothing of maps. The map
  is not in the status reply: the host wrote it, and the fault carries whether
  it is met.

## 9. Code

| File | Holds |
| --- | --- |
| `include/common.h` | the wire contract both sides build from: baud, commands, NAK reasons, flags, the status payload, the broadcast allowlist, the CRC |
| `src/rp2350/core1/bus/uart_9bit.pio`, `RS485Bus.*` | the controller's 9-bit port |
| `src/rp2350/core1/bus/packet.*` | frames, answers, broadcast, the e-stop sweep |
| `src/rp2350/core1/rpc_server.cpp` | one node transaction per RPC request |
| `src/rp2350/core1/core1.cpp` | the keep-alive, the e-stop sweep's order |
| `src/rp2350/core0/ops/bus.*` | the boot sweep, mute and excluded |
| `src/rp2350/core0/ops/slot_map.*` | the slot request and its apply |
| `src/rp2350/core0/ops/axes_map.*` | the axes request, the type checks |
| `src/rp2350/core0/ops/position.*` | the slot table, the fence, the axis views |
| `src/node/rs485/` | the node's framing, its generic receive interrupt, sending a frame |
| `src/node/main.cpp`, `dispatch.cpp` | the CRC check, silence, routing, NAK, make-safe |
| `src/node/types/stepper/stepper.cpp` | the stepper's receive interrupt and engage |
| `src/node/types/vacuum/probe_slot.h` | the probe's answer |
