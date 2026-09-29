# Bus frame sync

RS485 command framing between the Pico and the nodes: how a node finds frame
boundaries, and the fix for nodes losing them. Node-side dispatch is in
[../node_type_architecture.md](../node_type_architecture.md) §6.

## Frame

```
[id][cmd][len][payload…][crc]      every byte 9th bit = 1
```

* `id`: destination node, `0xFF` broadcast. A node reply carries its own id.
* `len`: payload bytes; the frame is `3 + len + 1` bytes.
* `crc`: CRC-8 over everything before it.
* A stream byte (9th bit = 0) ends any partial frame on every listener.

### Sync rule

* A frame ends at its length, whether or not it is addressed to the listener.
* A stream byte resyncs a parser that lost count (a dropped or garbled byte).
* Every Pico command frame is sent after a stream byte (`busQuiesce`); node
  replies carry their own stream-byte preamble.

### Field order

`[id][cmd][len]…` stays. Alternatives considered:

* `[len][id][cmd]…`: a framing layer independent of addressing, so a sniffer
  can split frames without knowing ids. Costs a protocol break on every node,
  the Pico and the web sim; the skip logic is the same either way.
* `id` first is what lets a node drop foreign traffic at byte 1, and only the
  addressed node stores anything. `crc` can only go last.

## Branch: `fix/bus-frame-sync`

**Type:** fix. **Depends on:** nothing.

**Purpose:** after `stop`, only the lowest-id node released its slot. A node
parser that saw a foreign frame discarded every command byte until a stream
byte, and the estop sweep sends frames without one, so each node after the
first dropped its own make-safe.

**Plan:**

* Pico, `src/rp2350/core1/core1.cpp:73`, `core1/bus/packet.cpp:66`: quiesce
  before the estop broadcast and before each `busStopAll` frame. `packet.h`:
  the `busQuiesce` comment states it is required.
* Node, `src/node/rs485/frame.h:12`: count every byte and end the frame at
  `3 + len + 1`; commit only when addressed, the ring has room, and it fits.
  Covers the foreign frame, a full ring (today it stops counting and merges
  the next frame into garbage) and `len` over `MAX_PACKET_LEN - 4` (today it
  never completes).
* Docs: this file; a pointer from `node_type_architecture.md` §6.

**Checks:** `pio run -e pico` and every node env. Human scope: after `stop`
every node reads `slot none`; a node parses its own frame straight after a
foreign one with no stream byte between.

**Status:** done (e982918..b76e496). Unblocks nothing.

**Outcome:**

* No deviations from the plan. `rxIdx` became `uint16_t`: a frame is up to
  259 bytes.
* Checks: `pio run -e pico` and all 19 node envs clean, no warnings.
* Bench: with the fix, every node reads `slot none` after `stop`. With the
  unfixed Pico (no quiesce) and only node 2 on the fixed parser, nodes 1 and 2
  released and 3, 4 kept their slots: the node fix alone recovers the frame
  after a foreign one. Still to flash: every other node.
* Out of scope:
  * `core1/bus/packet.h`: the `busStopAll` comment still names
    "CMD_DISABLE on the reset park"; estop is its only caller.
  * `docs/node_type_architecture.md:345`: a stray `| Reply | … |` table row,
    split from the command/stream table above it.
