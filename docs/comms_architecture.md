# Comms Architecture

**Branch:** `node-types`
**Date:** 2026-07-19
**Status:** DRAFT — transport substrate and link model written; firmware changes
and telemetry still to come.

How the host↔Pico link is structured, and why. `wire_protocol.md` remains the
source of truth for framing and constants; this doc covers architecture only.

---

## 1. The transport substrate: USB CDC-ACM

Host↔Pico rides a single USB CDC-ACM interface over a **bulk** endpoint pair.
Both sides open it as a virtual serial port, which makes it *look* like a UART.
It is not one, and the differences are load-bearing.

| Property | Reality (RP2350 = USB 1.1 full-speed only) |
|---|---|
| Raw rate | 12 Mbit/s → ~1.0–1.2 MB/s practical for bulk CDC |
| Quantum | 64-byte bulk packets |
| Latency | ~1 ms host polling frame → RTT is quantized to ~1 ms |
| Ordering | Guaranteed |
| Integrity | Hardware CRC16 + automatic retry per transfer |
| Flow control | TinyUSB NAKs the OUT endpoint when its FIFO fills; host retries |

**Baud is fiction.** `Serial.begin(115200)` and `serial.Serial(port, 115200)`
are both ignored — CDC-ACM passes line coding through as metadata only. The
number that *is* real is `RS485_BAUD` (921600), on the Pico↔ATtiny link, which
is a genuine UART with genuine line-rate and noise constraints. The two links
have opposite characteristics; do not carry discipline from one to the other.

### Consequences

- **The link is already reliable and flow-controlled.** Bytes arrive intact and
  in order, or not at all. A host outrunning Core 0 blocks rather than losing
  data — so Core 0 stalling is *safe*, though not *live*.
- **Therefore CRC8 + Go-Back-N is not error recovery** — those errors cannot
  occur here. Its real mandate is narrower: application-level backpressure
  (`NACK_FULL` on a full `masterBuf`, which USB flow control does not protect),
  desync recovery, and duplicate suppression after a Go-Back-N rewind. Optimizing
  the transport is safe because there is no error correction to weaken.
- **Cost is per-transaction, not per-byte.** A 3-byte write and a 60-byte write
  cost about the same. Throughput is governed by how many transfers we initiate.
  Any design emitting one small transfer per protocol event wastes the link.
- **Framing is ours.** CDC is a byte stream with no message boundaries; 64-byte
  packet boundaries carry no meaning. Structure exists only through our
  magic-anchored framing. No benefit to aligning to 64 bytes, no hazard in not.

---

## 2. The link model

### 2.1 One reader, for the connection's lifetime

The reader owns the port from connect to disconnect, demultiplexes inbound bytes,
and fans them out to typed sinks. Nothing else reads the port; sessions and
pollers **subscribe** rather than seize.

```
Connection (owns port)
  └─ DemuxReader ──► ack/nack sink   ← Session subscribes for its span
                 ──► status sink     ← StatusMonitor, always on
                 ──► text sink       ← ControlChannel
                 ──► cfg sink        ← config transactions
```

This is not a new invariant — magic-anchored framing exists precisely so any
response can be classified at a boundary. The demux makes one component
responsible for it instead of duplicating partial versions in every caller.

**It must be a state machine, not a magic scanner.** Not every inbound byte is
framing:

| Response | Length | Payload opaque? |
|---|---|---|
| `ACK` / `NACK` | 3 (fixed) | No |
| `STATUS_RSP` | 9 (fixed) | No |
| `CFG_RDY` / `CFG_ACK` | 1 | No |
| `CFG_NACK` | 2 | No |
| `CFG_DATA` | 9 + `length` | **Yes — arbitrary bytes** |
| text line | to `\n` | No (ASCII, bit 7 clear) |

A `CFG_DATA` payload is an opaque msgpack blob that **can contain `0xAA`,
`0xBB`, `0xA6`**. A scanner would emit a phantom ACK from inside config data,
advancing a session's `base` against a packet that was never sent —
blob-dependent, intermittent, and near-impossible to trace. So length-prefixed
payloads are consumed opaquely, scanning nothing. Fixed-length frames are safe to
consume blind; text is safe because binary magics have bit 7 set and control
replies are ASCII.

### 2.2 One writer, for a frame

Demultiplexing is hard; multiplexing is easy. The writer knows what it is
sending and needs none of the above. It has one job: **atomicity** — never let
two concerns interleave bytes within a frame.

The hazard is concrete. If a status poll writes `0xA5` mid-way through a 26-byte
MSEG, the firmware is in `rxKind = RX_FIXED26` and takes it **unconditionally as
packet payload** — there is no dispatch check mid-packet. The MSEG fails CRC and
the status request is silently eaten, hanging its caller to timeout.
`wire_protocol.md` already states the rule; today it holds *by accident*, as a
side effect of seizure. Once concerns write concurrently it must hold *by
construction*.

So ownership granularity is the mirror of the reader's: **the reader owns the
port always; the writer owns it per frame**, released between. That is what lets
a poll slot between two MSEGs without seizing anything.

**Batching is compatible.** The atomic unit is not one packet but a whole number
of frames: a 16-packet window write is one acquisition, one write, one flush —
416 bytes, still atomic, and per §1 that is where the throughput win lives. The
knob: larger batches trade telemetry latency for USB utilization. Batches must
stay bounded, or a writer holding the lock for a session reintroduces seizure.

**The writer does not read.** Today `_sendText` writes and immediately awaits a
reply. Here the writer only writes; the caller awaits its sink. That decoupling
is what allows a poll to be in flight while a stream is writing.

### 2.3 Correlation

> **Ordered channels correlate by position. Unordered request/response
> correlates by identity.**

- **MSEG/JOG is ordered** → the rolling seq, with cumulative ACK as its purest
  form: the reply echoes *how far I have got*, not *which packet this answers*.
  This is why one-reply-per-packet was safe to drop.
- **Status is a state sample, not a transaction result** → no correlation ID at
  all. Samples are **latest-wins**: a reply delayed past its caller's timeout is
  still a genuine, slightly older sample, self-correcting on the next poll.

**Nothing carries both.** Commit `138f7fb` removed a second rolling ID (`pktSeq`)
duplicating the wire seq; collapsing them is what made cumulative and coalesced
ACKs possible. Tagging MSEG would reintroduce it.

**No flushing, and no tag needed to avoid it.** Both hosts currently discard
queued bytes before a request — destructive and racy, since a reply legitimately
in flight dies like a stale one. That flush existed only because *one reader was
shared across concerns*: a stale status reply could be consumed as a text reply.
The demux removes that class of confusion structurally, by routing on magic to
dedicated sinks. What remains is intra-sink staleness, which latest-wins makes
harmless. A correlation tag would solve a problem the demux already solved.

The monitor keeps **at most one poll outstanding**, so delays cannot accumulate
a backlog of requests.

*Deferred:* a binary query that is **not** idempotent (per-node diagnostics,
where the answer's origin matters) does need correlation — but the discriminator
belongs in its payload (a node ID field), not a generic tag. Revisit only when
such a query exists.

*Rejected — CRC8 as a correlation ID:* identical requests produce identical CRCs,
so it cannot distinguish successive polls. It also welds integrity to identity,
and an 8-bit hash collision misdelivers a reply while looking valid. Requests are
events, not values.

**Per-plane policy:**

| Plane | Outstanding | Correlation |
|---|---|---|
| Stream (MSEG/JOG) | Many (window) | Position — rolling seq |
| Status | One | None — latest-wins state sample |
| Text | **One** | None — strict request/response |
| Config transaction | One | Structural (state-gated) |

`stop` is the exception to the text rule: it is **fire-and-forget**. It needs the
writer lock but not a reply slot, because the one-outstanding limit exists for
reply correlation and estop correlates nothing. Confirmation arrives on the
status sink as the state transitions to ESTOP→ALARM, so estop can never queue
behind a pending text command.

Text stays untagged and human-readable — it is low-rate, and `status` / `?` /
the bring-up CLI are meant to be typed at a terminal. Text commands needing
concurrency get *ported to binary* instead: `STATUS_REQ` is already the binary
mirror of `getstate`, and folding position into the status frame (§3) is the
same move applied to `getpos`.

### 2.4 Decisions

| | |
|---|---|
| **D1** | Single reader per connection, owning the port connect→disconnect. Nothing else reads it. |
| **D2** | Reader is a length-aware state machine. Fixed frames consumed blind; length-prefixed payloads consumed opaquely. Never scan payload bytes. |
| **D3** | Typed sinks; subscription, not seizure. |
| **D4** | Bulk reads — `read(max(1, in_waiting))`, not `read(1)` per byte. Same change as D1. |
| **D5** | Unknown bytes discarded individually (preserves resync) and counted. |
| **D6** | Frame-granular exclusive write. Atomic unit is a whole number of frames. |
| **D7** | Batches bounded — never hold the write lock for a session. |
| **D8** | The writer does not read; callers await a sink. |
| **D9** | Ordered channels correlate by position (seq). Status carries no ID — latest-wins. No frame carries two mechanisms. |
| **D10** | No flushing. Sink routing removes cross-concern confusion; latest-wins removes intra-sink staleness. At most one poll outstanding. |
| **D11** | Text stays one-outstanding and untagged; port to binary rather than tagging text. `stop` is fire-and-forget — writer lock, no reply slot. |
| **D12** | Sink API is designed for the browser's single-threaded async model. Python's reader thread is an implementation detail behind the same interface. |
| **D13** | A batch is abortable at any frame boundary. Urgent writes wait one frame, never a whole batch. Never abandon mid-frame on the normal path. |

### 2.5 Threading model

The browser is the constrained environment, so it sets the shape: **one
long-lived async read loop** over the WebSerial `ReadableStream`, feeding the
demux state machine, with sinks as awaitable queues (ack, text) and a
latest-value slot (status). The writer lock is a promise chain — the existing
`_serialize()` mechanism, but held per *frame* rather than per request-plus-read.

No Web Worker: throughput tops out around 1 MB/s (§1) and the demux is cheap, so
the main thread is sufficient, and Web Serial's availability in workers is not
dependable enough to rely on. If render work does starve the read loop, the cost
is **latency, not correctness** — USB backpressures, so bytes queue rather than
being lost (§1).

Python mirrors this interface with a reader thread and `queue.Queue` sinks. The
API is designed for the async model and threads implement it, not the reverse.

### 2.6 Open questions

- **Sink backpressure.** Unbounded queues hide a non-draining consumer. Status
  drops oldest (latest-wins); ack must never drop.
- **Fairness.** D7 bounds how long a poll waits, but bounded is not the same as
  usable — a saturating stream plus a 30 Hz poller needs measuring.
- **`stop` latency.** Estop still waits for the current batch's writer lock. It
  cannot preempt a partial write without violating D6, so a priority lane is
  deferred until measured.

---

## 3. Scenarios

How each concern behaves under the model. Note the absence of flushing
throughout — sink routing replaces it (D10).

**Text command.** Acquire writer → write `"pause\n"` → release. Then await the
text sink. The reply reached that sink because bit 7 was clear; it cannot be
confused with an ACK or status frame, which went elsewhere. One-outstanding
(D11) makes the next line in the sink unambiguously yours.

**Streaming MicroSegments.** Subscribe to the ack sink. Loop: acquire writer →
write a bounded batch of whole packets → flush → release; then await the ack
sink and advance `base` by the cumulative delta. The session never observes
status or text traffic.

**Live telemetry, concurrently.** The monitor runs its own loop: acquire writer
→ write `STATUS_REQ` (one byte) → release → await the status sink, at most one
poll outstanding. It needs the writer for a single byte between frames, so an
active session delays it by at most one frame (D13). This is the case that is
impossible under seizure.

**Estop.** Abort the batch at the next frame boundary rather than queueing
behind the rest of it (D13), then write `"stop\n"` and **do not await a reply**.
The one-outstanding text limit exists for reply correlation, and estop
correlates nothing — so it can never queue behind a pending `getstate`.
Confirmation arrives on the status sink as the state goes ESTOP→ALARM. Total
wait: one frame.

*Pathological fallback only:* if the write itself is blocked (Pico not draining,
host dying) the frame cannot be finished, and abandoning mid-frame leaves the
data plane wedged in `RX_FIXED26`. The inter-byte timeout (§4) recovers it. This
is a backstop, not the normal path — a wedged data plane eats *every* plane's
traffic for the timeout's duration, including the telemetry you most want during
an estop.

---

## 4. Firmware changes

Four changes, each traceable to a decision above. Only 4.2 and 4.3 require
matching host work.

### 4.1 Coalesced ACKs

Cumulative ACKs (`138f7fb`) removed the one-reply-per-packet obligation. Collect
the receipts instead of emitting one per packet: `feedFixed26` sets a pending
flag rather than calling `sendAck()`, and `processSerial` flushes once.

Rules, all load-bearing:

- **Flush on drain-empty OR pending ≥ K**, K ≈ 8 (half the window). Drain-empty
  alone is not enough — under a saturating stream `Serial.available()` may never
  empty, starving ACKs until the window stalls.
- **Flush any pending ACK *before* emitting a NACK.** Reversed, the host rewinds
  `next_send` first, so the late ACK computes `next_send - base = 0` and the
  window clamp rejects it — costing a redundant window of retransmission. Not a
  correctness bug (the clamp holds), but a pure waste the ordering avoids.
- **Never go idle dirty.** An unflushed batch at idle costs a full
  `ACK_TIMEOUT_S` and a spurious go-back.
- Emit the frame as one `Serial.write(buf, 3)`, not three byte writes (§1: cost
  is per-transaction).

Both hosts already decode an arbitrary delta, so this is unilateral and
backward-compatible — a coalescing Pico works against an un-updated host.

### 4.2 Extended status frame

Fold position and `expectedSeq` into the status response. Position currently
requires `getpos` on the text plane — the most constrained plane — and arrives as
a *separate* round trip, so state and position can disagree by tens of ms. One
frame makes the sample coherent. Per §1 the extra bytes are free: cost is
per-transaction.

`expectedSeq` rides along as an informational reconciliation field — it lets a
host resynchronise after a timeout, abort, or reconnect without guessing. It is
**not** flow control; ACKs remain the only advance mechanism (D9).

`STATUS_REQ` is unchanged (one byte, no tag — D10). The **response** takes a new
magic so a version mismatch fails cleanly as an unknown byte (D5) instead of
desyncing the reader, which consumes fixed-length frames blind (D2).

### 4.3 Binary `seqreset`

`seqreset` is text, and sits on the critical path of every stream start — the one
text round-trip a session cannot avoid. That drags an otherwise pure data-plane
session through the one-outstanding text plane.

Give it a binary twin: a one-byte magic that zeroes `expectedSeq` and replies
`ACK(0)`. Reusing ACK is exact — "I expect seq 0 next" — and keeps the session on
a single sink. Note the session must treat that ACK as an explicit readiness
signal rather than feeding it to the advance logic, which correctly ignores a
zero delta.

**Deliberately not folded into MCFG.** MCFG is job metadata (required_axes, later
a config CRC); seq reset is a data-plane control op. Coupling them would mean you
cannot reset the seq without declaring axes, or declare axes without resetting.
The text command stays as a bring-up alias (D11).

### 4.4 `RX_FIXED26` inter-byte timeout

`RX_CFG` has an inter-byte timeout; `RX_FIXED26` has none. A truncated packet
wedges the data plane until 26 bytes arrive, eating text and status bytes as
payload in the meantime.

Handle it in `dataPlaneTick` alongside the CFG timeout but with a **separate
constant** — a 26-byte packet arrives in microseconds, so its timeout wants tens
of ms, not `CFG_RX_TIMEOUT_MS`'s 2000. On expiry, reset `rxKind`/`pktIdx`
silently: no reply. A streaming host recovers via its existing ACK timeout, and a
host that abandoned mid-frame is not waiting for anything.

This is a **backstop for the pathological case** — host crash, disconnect, or a
blocked write — not the estop path, which truncates at a frame boundary (D13).

### 4.6 Wire changes

| Item | Change |
|---|---|
| `STATUS_RSP` | New magic; adds `pos[4]` (int32 LE) + `expectedSeq`. 9 → 25 bytes |
| `SEQRESET` | New one-byte host→Pico magic; replies `ACK(0)` |
| `STATUS_REQ` | Unchanged |
| ACK/NACK | Unchanged on the wire; emission cadence only |

Host and firmware must be flashed together for 4.2/4.3. The new magics make a
mismatch fail visibly rather than silently.

### 4.7 Sequencing

1. **4.1 and 4.4 first** — no host changes, independently verifiable.
2. Validate with the `integrity` test in `host/diagnostics/test_comms.py`: a
   known step count streamed slowly enough to force `NACK_FULL`, asserting
   `getpos` matches. It is the test that catches a botched batch flush
   (over-count) or wrong NACK ordering. Python first — it forces backpressure
   deterministically and reports counters; the web demo is a *regression check*,
   and should be a literal no-op.
3. **4.2 and 4.3** with their host counterparts, once the demux (D1–D5) exists —
   4.3 is pointless before a session can subscribe to a sink.

### 4.8 Possible follow-on: unsolicited status push

The demux does not care whether a frame was solicited, so the Pico could emit
status on state change, or at a rate while RUNNING — dropping the request half
entirely and reporting transitions faster than any poll interval. Needs rate
control, and shares the return path with ACKs. Consider only after 4.1 lands.