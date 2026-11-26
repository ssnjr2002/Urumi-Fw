# Wire

**Checked against `0d5cf2b`.** To see what may have changed since:

```bash
git diff 0d5cf2b -- src/rp2350/core0/core0.cpp src/rp2350/core0/data_plane.cpp src/rp2350/core0/usb_protocol.h web/src/link/
```

Commands, replies, status keys and code names are written as the
[notation](../README.md#notation) says.

The wire is the USB link between the host and the controller. One serial
stream carries five kinds of traffic: text commands and their replies, a
binary status reply, the job stream, continuous jog, and config transfers.
The controller's end is Core 0's byte routing and its data plane; the host's
end is the link library in `web/src/link/`.

This page is how an exchange runs at each end. The byte layouts, reply codes
and the order the controller checks a record in are in
[reference/packets.md](../reference/packets.md); the text commands are in
[reference/commands.md](../reference/commands.md).

## 1. Transport

The RP2350 is a USB full-speed device, and the link is one USB CDC serial
interface over a bulk endpoint pair. Both ends open it as a serial port, but
it is not a UART:

| Property | USB CDC |
| --- | --- |
| Rate | 12 Mbit/s, about 1 MB/s in practice |
| Quantum | 64-byte packets |
| Latency | about 1 ms, the host's polling frame |
| Order | guaranteed |
| Integrity | a hardware CRC and retry on every transfer |
| Flow control | the device refuses data while its buffer is full, and the host retries |

* **The baud rate sets no speed.** Baud rate is ignored unless it's set to
  1200, which reboots the controller into its bootloader. The
  `Serial.begin(115200)` and the host's 115200 are passed along as settings
  only.
* **Bytes arrive whole and in order, or not at all.** A host that outruns
  Core 0 waits; it doesn't lose data.
* **Cost is per transfer, not per byte.** A 3-byte write costs about what a
  60-byte write does.
* **The stream has no message boundaries.** The 64-byte packets mean
  nothing; frames exist only through the magic bytes and lengths of
  [packets.md](../reference/packets.md#framing).

## 2. One stream, two ends

### Controller

Core 0 reads every waiting byte in `processSerial()` (`core0.cpp`) and sends
it one of three ways:

1. **The data plane** (`dataPlaneConsume()`, `data_plane.cpp`) takes a byte
   that opens one of its messages, and every byte of a message under way. A
   message under way takes its bytes by count, without looking at them. The
   data plane has two receivers: one for the fixed-size records (the job's
   headers and `BEZIER`, and `CJOG`), and one for the config transfer, whose
   length comes in its header. `CFG_GET`, `ABORT`, `CJOG_STOP` and `SEQRESET`
   are one byte each and handled at once.
2. **`STATUS_REQ`** is one byte, answered at once with `STATUS_RSP`.
3. **Anything else is text**, an unknown byte with bit 7 set included. Bytes
   build a line until `\n` or `\r`; then `handleCommand()` runs it.

Between passes over the input, `dataPlaneTick()` sends held-back ACKs, ends
a stalled transfer, and drops a half-received record. Where this sits in
Core 0's loop is in [controller.md](controller.md).

### Host

The `Link` (`link.ts`) owns one `Transport`, the byte pipe to the
controller. It starts a **read loop** that passes every chunk to the
**demux** (`demux.ts`), which splits the bytes into frames and puts each on
a **sink** (`sink.ts`), one per kind of frame. Callers wait on a sink for
their reply. Everything sent goes through one **writer** (`writer.ts`).

The demux is a state machine. Between frames it reads one byte:

| Byte | The demux then |
| --- | --- |
| `ACK`, `STATUS_RSP`, `CFG_NACK`, `JOG_NACK` | reads the frame's fixed length, by count |
| `NACK` | reads its header (3 bytes with the magic), then the text and CRC8, `len` + 1 bytes, by count |
| `CFG_RDY`, `CFG_ACK`, `JOG_ACK` | has a whole frame already |
| `CFG_DATA` | reads its header (9 bytes with the magic), then `length` payload bytes, by count |
| ASCII (bit 7 clear) | reads a text line to `\n`, dropping a trailing `\r` |
| anything else | drops that one byte and counts it (`unknownBytes`) |

A chunk can end mid-frame; the state carries over to the next one. A
`CFG_DATA` payload is a msgpack blob and can hold any byte, magic bytes
included. A `length` over 64 KB (`MAX_CFG_PAYLOAD`) isn't read at all,
since it would swallow everything after it; the demux counts an overrun and
goes back to reading frames. So does a text line over 512 bytes, and a
`NACK` whose `len` is over 48 or whose CRC8 doesn't match.

| Sink | Takes | Kind |
| --- | --- | --- |
| `ack` | `ACK`, `NACK` | ordered |
| `jog` | `JOG_ACK`, `JOG_NACK` | ordered |
| `status` | `STATUS_RSP` | latest |
| `text` | text lines | ordered |
| `cfg` | `CFG_RDY`, `CFG_ACK`, `CFG_NACK`, `CFG_DATA` | ordered |

`Link`'s methods are grouped by plane: text, status, the job stream, jog
and config, each in a section below.

## 3. Decisions

* **One reader owns the port for the whole connection, and callers wait on
  sinks.** Rejected: each request reading its own reply, which needs the
  port to itself and stops a status poll while a job streams.
* **The demux routes by magic byte and length, reading payloads by count.**
  Rejected: scanning for magic bytes, which finds false frames inside a
  `CFG_DATA` payload, such as an ACK that moves a stream's window past
  records never sent.
* **Unknown bytes are dropped one at a time and counted.** Rejected:
  dropping to the next newline or magic byte, which can eat the start of a
  real frame.
* **No flushing before a request.** A stale reply lands in its own sink, not
  in the next caller's. Rejected: discarding the input before each request,
  which also discards replies still on their way.
* **Status keeps only the latest sample.** A late status reply is still a
  true, slightly older sample, and the next poll replaces it. Rejected: a
  queue, which hands a caller an old state.

* **The writer sends whole frames, and only one caller at a time.** A byte
  from a status poll inside a record would be read as part of the record.
  Rejected: taking the port for a whole stream, which stops polls and
  commands until it ends.
* **Records go out in batches of up to 16, one write each.** Rejected: one
  write per record, which pays a USB transfer per record.
* **The controller holds ACKs back, up to 8, and each one is cumulative.**
  One ACK confirms everything before it, so holding them back loses nothing.
  Rejected: one reply per record.
* **An ACK held back goes out before any NACK.** The host's window must
  move forward before it rewinds; the other way round, it would skip
  records it had just resent.

* **The job stream is matched by position, the seq; status by nothing.**
  Rejected: an id on every request, which the cumulative ACK doesn't need
  and a status sample has no use for; and the CRC as an id, which is the
  same for two identical requests.
* **Text is one command at a time, untagged, one reply line each.** It stays
  readable and typeable at a terminal. Rejected: tagging text to run
  several commands at once. What must run alongside other traffic becomes a
  binary message instead, as status did.

## 4. Writer

Host only. `Writer` (`writer.ts`) holds a lock, a promise chain, for each
write:

* `writeFrame()` sends one frame: a status request, a text line, a
  one-byte message.
* `writeBatch()` sends up to 16 frames joined into one write, and returns
  how many it sent. It takes an abort flag and checks it before writing: a
  set flag sends nothing and returns 0, so a stopped stream never leaves
  half a batch.

A caller waits for its turn; between two writes anyone may write. The writer
never reads: a caller that wants a reply waits on its sink.

## 5. Text commands

### Controller

A line is at most 255 characters; longer ones are cut. Every command
answers with exactly one line, `ok …` or `err …`, and an unknown one with
`err unknown`. One exception: after `reset`'s `ok` the controller restarts
and prints its start-up lines, ending with `ready`. The commands and their
replies are in [commands.md](../reference/commands.md).

### Host

* `Link._command(text)` sends a line and waits for the reply, 1 s by
  default; on timeout it returns `""`. One command is outstanding at a
  time. Replies come back in the order the lines went out, so the link
  keeps a list, in that order, of who each awaited line belongs to.
* `estop()` sends `stop` at once, never behind a command waiting for its
  reply. Its `ok` goes on the list as a line nobody reads, and is dropped
  when it arrives.
* `reset` reads its `ok`, then drops lines through `ready`.
* Before sending, a command empties the text sink. A line still there is
  an estop's `ok`, dropped, or left over from a command that timed out or a
  reply of more than one line, counted in `textDesyncs`, which should stay
  0.
* `commands.ts` holds `Commands`, from `link.commands()`: one method per
  command, each parsing its reply and throwing `CommandError` on a refusal.
  `onReady` waits for the status's `busy` to be false before sending.

## 6. Status

### Controller

`STATUS_REQ` is answered at once, between two records if a job is streaming.
The reply, `STATUS_RSP`, describes one moment on the controller. Its fields
are in [packets.md](../reference/packets.md#status).

### Host

* The link sends `STATUS_REQ` every `statusPollMs` on its own.
* `status` is the latest sample, with no round trip; `freshStatus()` waits
  for the next one.

A job session polls status every 100 ms while it streams, after each batch.

## 7. The job stream

How a job runs, and how the job session holds through pauses and tool
changes, is in [jobs.md](jobs.md). This section is how the records cross.

### Controller

* **The seq.** Each record carries a seq byte. The controller keeps
  `expectedSeq`, the seq it will run next. A record with any other seq is a
  resend of one already taken: it is ACKed at once and not run, so a resend
  never moves the machine twice.
* **The ACK.** An ACK carries `expectedSeq`, which confirms every record
  before it. Taking a record counts one more ACK owed (`pendingAcks`); the
  ACK goes out when 8 are owed (`ACK_COALESCE_MAX`), when the input is empty
  (`dataPlaneTick()`), or just before a NACK.
* **A NACK** refuses one record, with a reason and, on a reason that ends
  the job, a short text naming the check that failed. The record isn't taken
  and `expectedSeq` stays.
* **`SEQRESET`** sets `expectedSeq` to 0, drops the ACKs owed, and answers
  `ACK(0)` at once.
* **`ABORT`** asks Core 1 to stop the job; there is no reply. Until the
  job has ended, every job record gets `NACK_ABORTING`. `CJOG` is checked
  before this, so a jog can still brake.
* **A half-received record** with no byte for 50 ms
  (`PACKET_RX_TIMEOUT_MS`) is dropped without a reply.

### Host

* **A seq reset.** A session sends `SEQRESET` and waits for its `ACK(0)`
  before a job and before each resume, so every run of frames starts from
  seq 0.
* **The window.** A session keeps up to 16 records in flight. Each record is stamped with the next seq as it enters
  the window, and kept until it is ACKed. A resend comes from that copy.
* **An ACK** moves the window forward by the difference between its seq and
  the oldest record in flight, counted mod 256 and capped at the number in
  flight, so a repeated or stale ACK moves nothing.
* **A resend** starts again from the oldest record in flight: after 200 ms
  without a reply, after `NACK_CRC`, and after `NACK_FULL`, `NACK_BUSY` or
  `NACK_ABORTING` with a 50 ms wait.
* **`abort()`** sends `ABORT` without waiting.

The job session (`Session`, `session.ts`) is the one sender every job uses.
Which NACKs make it hold or give up is in
[jobs.md](jobs.md#7-the-hosts-job-session).

## 8. Continuous jog

### Controller

A `CJOG` packet carries a direction for each axis and a speed. A new
direction answers `JOG_ACK` once the jog starts, which may be a few passes
later, from `dataPlaneTick()`, if it waits for the ring to drain; or `JOG_NACK`
and a reason. A repeat of the running direction gets no reply, and must come
within 150 ms (`CJOG_DEADMAN_MS`) or the jog stops. All directions zero, or
`CJOG_STOP`, stops it, with no reply. What a jog does is in
[machine/positioning.md](../machine/positioning.md).

### Host

`cjog()` sends one packet; a caller repeats it faster than 150 ms while the
operator holds the jog. `cjogStop()` sends `CJOG_STOP`. Both send without
waiting; the replies arrive on the `jog` sink.

## 9. Config transfer

### Controller

A push (`CFG_SET`) comes in two steps: the header, answered `CFG_RDY`, then
the payload, answered `CFG_ACK` once it is stored. A transfer with no byte
for 2 s (`CFG_RX_TIMEOUT_MS`) is ended with `CFG_NACK_TIMEOUT`. A pull
(`CFG_GET`) is answered with `CFG_DATA`, the whole blob, before Core 0 reads
anything else. Both are in [config.md](config.md#4-wire).

### Host

`pushConfig()` sends the header, waits for `CFG_RDY` (1 s), sends the blob,
and waits for `CFG_ACK` (5 s, for the flash write). `pullConfig()` sends
`CFG_GET` and checks the blob's CRC32. Both take one lock, so one transfer
runs at a time, and throw `ConfigTransferError` on a NACK or no reply.

## 10. Transports

Host only. A transport implements `Transport` (`transport/index.ts`): `write()`,
`read()` as an async stream of chunks, and `close()`.

| Transport | Runs in | How to get it |
| --- | --- | --- |
| WebSerial | a browser | `WebSerialTransport.request()` or `fromPort(port)`, from `urumi-host/transport/webserial` |
| Node | Node.js, through the `serialport` package, which the program installs | `new Link(await NodeSerialTransport.open(path))`, from `urumi-host/transport/node` |
| Sim | anywhere | `new SimTransport(options)`, from `urumi-host/transport/sim` |

Each transport is its own subpath, so a Node program never
loads browser code and a browser never loads `serialport`. The full
interface is on [transport](../reference/host-api/transport.md).

**The Sim** (`transport/sim.ts`) is a fake controller for tests and the demo.
It answers its replies straight into the demux, which the `Link` hands it
through `attach()`. It models:

* the seq guard, coalesced ACKs and `NACK_FULL` on a full ring;
* a job: the headers, buffering, records run at a fixed rate, pause,
  resume, cancel and `ABORT`, and, with the `tools` option, tool changes;
* step and continuous jogs, homing legs, the axis map, and the text
  commands its tests use.

It doesn't model planning, timing, config transfers, or a bad CRC, which it
drops without a NACK.

## 11. Lifecycle

### Controller

At boot and at every soft reset, Core 0 throws away the waiting input,
clears both receivers, and sets `expectedSeq` to 0. It then prints a banner
and, once the bus sweep is done, `ready`. A config push ends in a soft
reset, so a host sends nothing after `CFG_ACK` until `ready`.

### Host

`new Link(transport)` starts the read loop at once. When the port closes or
the cable is pulled, the read loop ends and `closed` is set; a caller
waiting on a sink gets its timeout. `close()` closes the transport.

## 12. I/O

### Host to Core 0

| Message | Plane | Reply |
| --- | --- | --- |
| a text line | text | one text line |
| `STATUS_REQ` | status | `STATUS_RSP` |
| `JOB_HDR`, `TOOL_HDR`, `LAYER_HDR`, `BEZIER` | job stream | `ACK` (cumulative, may be held back) or `NACK` |
| `SEQRESET` | job stream | `ACK(0)` |
| `ABORT` | job stream | none |
| `CJOG` | jog | `JOG_ACK` or `JOG_NACK` on a start or refusal |
| `CJOG_STOP` | jog | none |
| `CFG_SET` header, then payload | config | `CFG_RDY`, then `CFG_ACK`; or `CFG_NACK` |
| `CFG_GET` | config | `CFG_DATA` |

### Core 0 to host

Only replies, and the banner and `ready` at boot. The controller sends
nothing unasked.

## 13. Who writes what

| Value | Written by | Read by |
| --- | --- | --- |
| `expectedSeq` | the data plane: each record taken, `SEQRESET`, a soft reset | the seq guard, each ACK |
| `pendingAcks` | the data plane | `dataPlaneTick()`, `sendNack()` |
| the record and config receivers | `dataPlaneConsume()`, `dataPlaneTick()` | the data plane |
| the text line | `processSerial()` | `handleCommand()` |
| the sinks | the demux | `Link` methods, sessions |
| the window (`base`, `next`, `emitted`) and the seq | the session | the session |
| the text, config and writer locks | `Link`, `Writer` | their callers |
| `textDesyncs`, `unknownBytes`, `overruns` | `Link`, the demux | diagnostics |

## 14. Code

### Controller

| File | Holds |
| --- | --- |
| `src/rp2350/core0/core0.cpp` | `processSerial()`, the text line, the soft reset |
| `src/rp2350/core0/data_plane.cpp` | the receivers, the seq guard, ACKs and NACKs, `dataPlaneTick()` |
| `src/rp2350/core0/usb_protocol.h` | the magic bytes, sizes, reasons and timeouts |

### Host

| File | Holds |
| --- | --- |
| `web/src/link/link.ts` | `Link`: the read loop and a method set per plane |
| `web/src/link/demux.ts` | the demux and the frame classes |
| `web/src/link/sink.ts` | `Sink`, `LatestSink` |
| `web/src/link/writer.ts` | `Writer` |
| `web/src/link/session.ts` | the job session and its window |
| `web/src/transport/index.ts` | `Transport` |
| `web/src/link/commands.ts` | `Commands`: one method per text command, `CommandError` |
| `web/src/transport/` | WebSerial, Node, the Sim |
| `web/src/packets/` | the packers and parsers, as in [packets.md](../reference/packets.md) |
