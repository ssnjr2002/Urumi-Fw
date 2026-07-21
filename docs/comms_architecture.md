# Comms Architecture

**Branch:** `node-types`
**Date:** 2026-07-19
**Status:** Link model implemented in Python (`host/protocol/`: `reader.py`,
`writer.py`, `session.py`) and driving the Tk UI's jog panel. Firmware §4.1 and
§4.4 landed; the rest specified and unblocked. TS port not started.
**§5 is the running ledger — read it first.**

Supersedes `state_redesign.md` on one point: resume is position-based, not
seqnum-based (§4.1, §4.5).

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

This is not hypothetical. Replaying a hostile-but-legal `CFG_DATA` payload
through the old `_ack_reader` scan loop emits **two** phantom ACKs, one claiming
`expected_seq = 255` — enough to slam a session's window to the end of the job.
It is latent today only because `Sender` seizes the port, so a `CFG_GET` cannot
overlap a stream; removing the seizure is what would arm it. Both the failure and
the fix are pinned by `test_cfg_data_payload_full_of_magics` in
`host/diagnostics/test_reader.py`.

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

### 2.3 Two kinds of session

A session is a subscription to the ack sink for a span of stream packets. Two
kinds exist, and they differ in a way that shapes the API rather than just the
usage.

**Closed** — a job. The full packet sequence is known before the first byte goes
out. The session is *given* an iterator and runs to exhaustion; it ends because
it ran out of packets.

**Open** — **manual jogging: the operator driving the machine in real time from a
jog panel.** One click commits one fixed distance; clicking again mid-motion
*extends* the live move rather than queueing a second burst. The total is not
known when the first byte goes out, because it depends on clicks that have not
happened yet. The session is *fed* — an intent arrives from the UI thread
mid-flight — and ends when the distance is spent, or by **truncation** on
reversal or estop.

The consequences are concrete, and `host/diagnostics/jog_blend_ui.py` shows what
their absence costs:

- **Blending is window continuation, not burst concatenation.** Today each burst
  builds a fresh `Sender`, seizing and releasing the port per phase, so a blend
  is really "did the next seizure land before the buffer drained." One open
  session with continuous seq makes it an append. `seqreset` fires once at the
  start, not per burst.
- **Deceleration is decided against live telemetry.** The current
  `_wait_for_buffer_low` can only poll *between* bursts, because sending owns the
  port — so the blend-vs-decel decision is made on a sample stale by the whole
  duration of the send just completed. Chunking cruise into ~10 ms packets exists
  solely to make `buf_count` move often enough for that gap-sampled poll to see
  it. Under a subscription the status sink keeps updating *during* transmission,
  which removes the blind spot and with it the reason to chunk.
- **Reversal and estop are the same primitive.** Both are D13 frame-boundary
  truncation; only the follow-up frame differs. "Keeping the decel packet off the
  wire until the last possible moment" is currently implemented by *not sending
  anything and waiting*; truncation makes it literal, bounded by one frame rather
  than by whatever was already handed to `send_stream`.

An open session therefore needs an input the closed one does not: a source of
intents it can poll without blocking, distinct from the packet queue. Conflating
the two is what forces `jog_blend_ui.py` to reach into `jog_q.mutex` and hand-roll
peek-and-clear against the UI thread.

What stays application logic: the blend-vs-decel *policy*, and the choice to hold
the decel packet back. The model makes those inputs current and the outputs
promptly actionable; it does not make the decision.

#### The signature

Implemented in `host/protocol/session.py`, validated by
`host/diagnostics/test_session.py` against an in-process fake Pico.

```python
class Session:
    def __init__(self, writer, ack_sink, source, status_sink=None, window=16): ...
    def run(self) -> bool: ...        # False only on fatal; truncation returns True
    def truncate(self) -> None: ...   # thread-safe, ends at next frame boundary

class PacketSource:
    def pull(self, ctx: StreamContext) -> list[bytes] | None:
        """None → finished   [] → nothing right now, still open   [pkt] → emit"""
```

Both session kinds fall out of one signature once **the retransmit buffer is
separated from the packet source**. In `Sender` these were the same list, doing
both jobs — which is precisely why it could only accept a closed sequence. The
session now retains its own window for replay and pulls from the source only for
new material, which yields the property that makes open sessions tractable:

> **`pull()` is called at most once per packet, ever.** A go-back replays from
> the retained window, never from the source.

So a source needs no idempotency and no memory of what it already produced —
exactly what a jog source cannot provide. Measured: 50 packets under forced
backpressure produced 266 writer sends and 16 go-backs, with `pull()` yielding
each packet exactly once, in order.

The three-way return is the whole distinction. `ListSource` returns slices then
`None`, and never `[]`. A jog source returns `[]` freely — the machine's queue
is full enough for now — and `None` only once the distance is spent.

**`ctx` carries live telemetry, not a round trip.** `ctx.buf_count` reads the
global status sink, which keeps updating *during* transmission. That is
`_wait_for_buffer_low` collapsing from a port-owning poll loop into a field read.

#### Three things practice changed

**The status monitor is a precondition for open sessions, not an add-on.**
Without it `ctx.buf_count` is `None` forever, so an open source can never decide
to idle and will happily outrun the machine. A closed session does not care. This
makes the always-on monitor load-bearing rather than merely useful, and it is why
`Session` takes `status_sink` at construction.

**Termination must test packets *emitted*, not packets *sent*.** After a go-back
the send cursor rewinds to `base` while the retained window still holds
pulled-but-unsent packets; testing the send cursor strands them and — worse —
exits reporting success. Caught by the backpressure test: 48 of 60 packets
executed, `run()` returned `True`. Silent truncation of a job, and exactly the
class of bug the fake-Pico harness exists to catch.

**An open source needs a bounded wait.** Returning `[]` with an empty window
leaves the loop nothing to await — the ack sink is empty *because* nothing is in
flight — so it spins. `ctx.wait(timeout)` is the hook; sources should block on
their own intent queue instead, or jogging gains a latency floor equal to a poll
interval. This is the one place Python and TS genuinely diverge (condition
variable vs. promise race), so it is part of the interface rather than the
implementation.

**"Busy" must mean *machine moving*, not *packets delivered*.** The session
originally ended on the last ACK, while the machine still had a ring full of
motion — so a click 50 ms later found no live session and started a fresh one
instead of blending. An open session stays open while the queue drains. Until
§4.6 this is a host-side estimate; §4.6 makes it a reported fact.

### 2.4 Correlation

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
mirror of `getstate`, and folding position into the status frame (§4.2) is the
same move applied to `getpos`.

### 2.5 Decisions

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
| **D14** | Sessions are closed (given an iterator, end by exhaustion) or open (fed intents, end by truncation). Manual jogging is open. The session API must support both — an open session takes a non-blocking intent source separate from its packet source. |

### 2.6 Threading model

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

### 2.7 Open questions

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

Each traceable to a decision above or to a workload that exposed it. 4.1 and 4.4
are unilateral; the rest need matching host work.

These extend `state_redesign.md` rather than revising it — new `RunningReason`
values and new request flags are exactly what its layered model prescribes for
new sub-modes and inter-core requests. Where a proposal *does* touch a decision
that doc already resolved, §4.5 says so explicitly.

### 4.1 Coalesced ACKs — **implemented**

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

Landed in `data_plane.cpp` as `markAck()` / `flushAck()`, `ACK_COALESCE_MAX = 8`
in `shared.h`. The drain-empty flush is `dataPlaneTick()`, which runs after
`processSerial()` has emptied `Serial.available()`. The stale-seq duplicate ACK
flushes immediately — it is the host's resync signal, not a receipt. `SimBackend`
mirrors all of it so the host suites exercise multi-packet advances.

**Measured on hardware** (600 packets @ 4000 sps, no backpressure): 75 ACK
frames for 600 accepted packets — exactly 8.00 per frame, an 8× cut in
return-path transactions. Note *which* trigger fired: the K counter, essentially
every time. A saturating host keeps `Serial.available()` non-empty, so
drain-empty almost never fires mid-stream — the predicted starvation is real,
and K is what prevents it, not a safety net.

**An ACK means *accepted into the ring*, never *executed*.** Coalescing makes
this obvious but does not cause it: with a 512-deep `masterBuf`, an ACK has
always been able to lead execution by the whole buffer. Any contract of the form
"last ACKed seq = last segment executed" is therefore unsound at any ACK cadence.
Position is the only witness of what actually moved — which is what §4.5 leans on
and §4.2 makes cheap.

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

### 4.3 Binary `seqreset` — **implemented**

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

Landed as `SEQRESET_MAGIC = 0xA8`, dispatched synchronously in
`dataPlaneConsume` with no receive state. `Link.reset_seq()` writes the byte and
**drains the ACK itself** rather than leaving it for the session — a session
opening on a stale ACK in its sink would advance its window against a packet it
never sent. Measured at 0.3 ms round trip on hardware.

### 4.4 `RX_FIXED26` inter-byte timeout — **implemented**

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

Landed as `FIXED26_RX_TIMEOUT_MS = 50` in `shared.h`, checked in `dataPlaneTick()`.

### 4.5 Soft abort — decelerate, flush, keep position — **firmware implemented**

> Landed: `EmitResult` + counted `out[4]`, the in-emitter ramp, `abortRequested`,
> `RUNNING_ABORT_DECEL`, `ABORT_MAGIC = 0xA9`, `NACK_ABORTING = 0x07`. Pause now
> routes through the same ramp; `MSEG_FLAG_PAUSE` still stops at the segment edge,
> being a *planned* boundary the host already decelerated into.
>
> Two deliberate stubs, both blocked on the same missing piece — **Core 1 has no
> config-read path**: the decel rates are four `#define`s (`DECEL_SPS2_X…A`,
> seeded from `web/demo/config.json` as `maxAccel × stepsPerUnit`) rather than
> config values, and `rampStepInBounds()` is a harness that always passes. The
> `EMIT_SOFT_LIMIT` path around it is fully wired, so enabling the check is a
> one-function change. Fix both together. **Z's rate is a placeholder** — that
> axis has no `maxAccel` in the config at all.
>
> Rate is selected by **major-axis index**, since that is the axis `interval`
> describes and the one Bresenham measures against. The seeded values span
> 22730 (A) to 160000 (X/Y) — stopping distance is v²/2a, so a 20 kHz move stops
> in ~1250 steps on X but ~8800 on A. No single global could have served both.
>
> Host `abort()` and the deletion of `_decel_distance()` are still to do.

Today there is no graceful stop. `cancel` requires `PAUSED`; `stop` goes
`ESTOP → ALARM`, invalidating position and demanding `unalarm` + `setorigin`. So
motion already in `masterBuf` cannot be called back — which is why the host plans
its own deceleration and `Session.truncate()` only stops *writing* while the
queued motion runs to completion.

**Soft abort:** ramp to rest from the current velocity, discard the rest of the
ring, land in `IDLE` with position intact.

`state_redesign.md` already specifies this mechanism for a different trigger —
*"when a new jog arrives during `STATE_RUNNING + RUNNING_JOG`, Core 1 performs a
controlled decel ramp before starting the new move."* So this is not a new
concept; it is that ramp, made addressable by the host.

#### Core 1 owns it, and there is no emitter to write

Only Core 1 knows the instantaneous velocity. A Core 0 generator would have to
ask over the FIFO, plan a decel, then flush-and-append — and Core 1 has moved on
by the time it does.

Core 1 consumes MicroSegments but **its output is stream bytes**, so nothing new
is produced: `emitMicroSegment` already varies nothing but the interval, and the
ramp just makes that interval a variable. Two phases fall out of one loop:

- **Within the segment** — stretch the interval per step. `dirBits` and the
  Bresenham accumulators are already correct, so the machine stops *along* the
  current path rather than on a newly planned one.
- **Past it** — if the step count runs out while still above rest, drop the
  `s < maxSteps` bound and keep stepping the same vector until at rest. That
  deletion *is* phase 2; there is no second function.

Per step, `v² ← v² − 2·a·d` with `d` = one major-axis step gives an exact
stopping distance regardless of where the ramp began. Decel rate comes from the
config blob.

#### Position must be counted, not assumed

`emitMicroSegment` returns `bool` and the caller adds `ms.dx…ms.da` to
`machinePos` **wholesale**. On estop it returns `false` and the caller skips the
accumulate — so steps physically emitted before the cut are never counted. *That*
is why `ESTOP → ALARM` invalidates position.

A ramp stops mid-segment and may overshoot it, so neither the planned delta nor
zero is right. The emitter must report what it actually emitted:

```c
enum EmitResult { EMIT_DONE, EMIT_RAMPED, EMIT_ESTOP };
static EmitResult emitMicroSegment(const MicroSegment& ms, int32_t out[4]);
```

`out[]` is accurate on every path including estop — the caller simply discards it
there, since estop forfeits position by choice, not by necessity. If estop should
ever stop costing a re-home, that is a call-site change, not a rework.

#### Request flag, RunningReason, barrier

All three follow `state_redesign.md`'s layered model rather than inventing
anything: *"new running sub-mode → new `RunningReason` value, not a new state"*
and *"new inter-core async request → new request flag + Core 1
drain-and-transition"*. `cancelRequested` is already listed alongside
`pauseRequested` as precedent.

| Request flag | Enacted by Core 1 | Result |
|---|---|---|
| `pauseRequested` | → `PAUSED` | resumable, position kept |
| `abortRequested` | → `RUNNING_ABORT_DECEL` → `IDLE` | position kept |
| *(estop)* | → `ALARM` / `ALARM_ESTOP` | stop dead, position forfeited |

`RUNNING_ABORT_DECEL = 2` is a `RunningReason`, not a state: the machine *is*
running, so every existing `IDLE/RUNNING/PAUSED` gate stays correct untouched and
an un-updated host reads it as plain RUNNING — which is true.

**Abort is a barrier.** Packets arriving during the ramp are NACKed with a
distinct `NACK_ABORTING`, not `NACK_BAD_STATE`. Accepting them would mean
blending into a deceleration and ramping back up from an arbitrary velocity, at
which point abort stops meaning anything definite. The distinct code is what makes
it good UX rather than an error — the host waits for IDLE and reopens instead of
surfacing a failure, and with §4.10 that IDLE arrives the instant it is true.

> **Everything sent before the abort is discarded; everything after waits for
> IDLE.**

A post-abort session must `seqreset` — but so must every other session, for the
unremarkable reason that sessions stamp from 0. Flushing the ring does *not*
invalidate `expectedSeq`; it keeps counting and remains a valid duplicate guard.
Abort adds no special requirement here.

#### Three interactions, resolved

**Pause-with-ramp is fine, because resume was never seq-based.**
`state_redesign.md` describes resume as continuing from the last ACKed seqnum + 1.
Per §4.1 that contract cannot hold — an ACK means accepted, not executed — so it
was already unsound before ramping entered the picture. Phase 1 resume is
host-driven and position-based (`resumePos` + `getpos`) precisely because of
this. So: **pause ramps, flushes, and resume re-plans from position.** That
supersedes the seqnum wording in `state_redesign.md` §PAUSE and matches what the
host already does.

**Soft-limit overshoot alarms, like any other violation.** Ramp phase 2 emits
steps beyond the planned delta, so it can cross a bound the Layer 3 per-emit
check already passed. Rather than reserve stopping distance (which needs
velocity at check time), the ramp keeps checking `inBounds()` per step and calls
`setAlarm(ALARM_SOFT_LIMIT)` if it crosses. Position stays counted and valid,
recovery is the existing unalarm-and-back-off path, and nothing new is invented.
Reserving headroom in the planner is a Phase 2 refinement.

**`STATE_ESTOP` stays.** It works, it is documented as deliberate, and abort does
not require touching it. Noted only so the next reader knows the justification
(*"removing it would require a separate signalling mechanism"*) is now weaker
than when written.

#### Cost

RP2350's Cortex-M33 has a hardware single-precision FPU (FPv5-SP) — unlike the
M0+ idioms this codebase inherited. `VADD`/`VMUL.F32` are ~1 cycle, `VDIV`/`VSQRT`
~14, against a ~5000-cycle step budget at 150 MHz and ~30 k steps/s: well under
1%, and only while aborting.

Doubles are **not** software-emulated — RP2350 has a separate DCP coprocessor —
but they are far slower than floats and involve CPU↔coprocessor transfers. In C a
bare `1.0` is a `double` and silently promotes the expression around it, so every
literal in the step loop needs an `f` suffix (`sqrtf`, not `sqrt`). Also budget
**72 bytes of extra stack**: lazy FPU stacking reserves it on first FP use and
keeps it reserved for later interrupts.

Guard the ramp against a zero decel rate from config, or the loop never
terminates. Phase 2 must re-check the estop condition every step.

#### What it replaces on the host — narrower than first claimed

Jog **cancel** becomes one byte instead of a coast: the Pico ramps from its
actual instantaneous velocity, which is the only place that value exists, and
can call back motion already sitting in the ring — which the host never could.

But `_decel_distance()` and the ramp-down branch **stay**. The firmware ramp
fires on abort/pause only; natural completion has no trigger, so deleting the
host ramp would leave the last packet running at feed speed and the machine
stopping dead. Aborting early instead would break the exact-distance promise
that makes a click-jog mean "10 mm".

So deceleration is split by cause: **runs to distance → host ramps** (the
distance must come out exact); **cancelled → Pico ramps** (exactness is
irrelevant, responsiveness is not).

This is the only proposal here that is not nearly free: Core 1 must ramp
intervals rather than merely consume them.

### 4.6 Queued motion time in STATUS_RSP

`buf_count` counts segments, but segments have wildly different durations. What a
jog source actually needs is *how many milliseconds of motion are queued*.
Maintain a running `queued_us` sum on ring enqueue/dequeue — a couple of adds in
an existing path.

**Folded into `STATUS_RSP`, not a separate command.** The argument is coherence,
not byte budget: a separate poll makes queued-time and state two round trips that
can disagree by tens of ms, and a source pacing against a `queued_us` that does
not match its paired `state` is exactly the bug that is invisible on the
simulator and intermittent on hardware. The budget merely confirms it — ~29 bytes
with §4.2's position, one USB packet, and since cost is per-transaction (§1) a
separate command doubles the expensive part to save 4 bytes of the free part.

Whole-segment granularity is fine: report the ring's total including the
executing segment without subtracting its elapsed time. Error ≤ one segment
(~20 ms at the host's current chunk size), well inside what pacing needs.

**What it actually replaced on the host — this claim was wrong.** The original
text said `queued_us` deletes `LEAD_S` / `_queued_s` / `_t0`. It does not. Those
exist because the report is only as fresh as the last status poll (~100 ms)
while `pull()` runs as fast as the ack loop allows; reading a stale value
between polls dumps the whole move onto the wire in one go. `queued_us` fixes
*which quantity* is reported, not *how often*. Deleting the wall clock on that
promise broke blending immediately, caught by `test_ui_jog`.

What it does fix is the question the estimate was worst at: **"is the machine
still moving?"** — where being wrong let `busy` go false mid-motion. So the
report is authoritative for drain detection, and pacing takes the **max** of
report and local estimate: a stale-low report cannot cause a dump, a stale-high
one cannot cause a stall. §4.10 (unsolicited push) would shrink the staleness
window but not close it.

### 4.7 Dead wire surface

**`MSEG_FLAG_PATH_END` — delete it.** It is set by five host call sites and read
by nothing: not `core1.cpp`, not either host. It has survived this long by
looking meaningful.

The jog work is the evidence that closes the question. An open session has no
final packet to mark — it ends by truncation, and the operator decides when. A
flag meaning "this is the last one" cannot be set by a source that does not know
whether the next click is coming, so the one workload that might have wanted an
end-of-motion marker structurally cannot use it. Meanwhile §4.5 gives the
firmware a real end-of-motion signal (`ABORT` → ramp → IDLE) and §4.10 reports
arrival at IDLE without asking, which covers what a reader would have wanted it
for.

Dead surface in a frozen contract is worse than either implementing or removing
it: every future reader must work out that it means nothing. Same applies to the
declared-but-unimplemented **MCFG** and **TILE/TOOL** magics — `transport.js`
already carries a comment saying MCFG "will cause bugs if left in".

### 4.8 Wire changes

| Item | Change |
|---|---|
| `STATUS_RSP` | New magic; adds `pos[4]` (int32 LE) + `expectedSeq` + `queued_us` (u32 LE). 9 → ~29 bytes |
| `SEQRESET` | New one-byte host→Pico magic; replies `ACK(0)` |
| `ABORT` | New one-byte host→Pico magic; enters `RUNNING_ABORT_DECEL` |
| `RUNNING_ABORT_DECEL` | New `RunningReason = 2` (no new `MachineState`) |
| `NACK_ABORTING` | New NACK reason — stream rejected, retry when IDLE |
| `STATUS_REQ` | Unchanged |
| ACK/NACK | Unchanged framing; emission cadence only (4.1) |

Host and firmware must be flashed together for 4.2/4.3/4.5/4.6. The new magics
make a mismatch fail visibly rather than silently; `RUNNING_ABORT_DECEL` is the
one item an un-updated host tolerates, reading it as plain RUNNING.

### 4.9 Sequencing

1. **4.1 and 4.4 first** — no host changes, independently verifiable.
2. Validate with the `integrity` test in `host/diagnostics/test_comms.py`: a
   known step count streamed slowly enough to force `NACK_FULL`, asserting
   `getpos` matches. It is the test that catches a botched batch flush
   (over-count) or wrong NACK ordering. Python first — it forces backpressure
   deterministically and reports counters; the web demo is a *regression check*,
   and should be a literal no-op.
3. **4.2 and 4.6 together** — both extend `STATUS_RSP`, so they are one wire
   change and one host-side parse change. Doing them separately means two
   flag-day migrations for one frame.
4. **4.3** once the demux (D1–D5) exists — pointless before a session can
   subscribe to a sink.
5. **4.5 last.** The only item touching Core 1's step loop and the only one not
   nearly free. Land the observability changes first so the ramp can be watched
   while it is debugged.

Jog is the workload that exercises all of these at once, so
`host/diagnostics/test_ui_jog.py` is the integration check: each of 4.2, 4.5 and
4.6 should *delete* host code (named in their sections) while that suite keeps
passing unchanged. If a change adds host code instead, the firmware side did not
land the responsibility.

### 4.10 Possible follow-on: unsolicited status push

The demux does not care whether a frame was solicited, so the Pico could emit
status on state change, or at a rate while RUNNING — dropping the request half
entirely and reporting transitions faster than any poll interval. Needs rate
control, and shares the return path with ACKs. Consider only after 4.1 lands.

---

## 5. Status

Ordered by §4.9's sequencing, not by section number. Sections above are the
specification; this is the ledger.

### Done

- [x] **§2.1–2.2 — demux reader + frame-atomic writer.** `host/protocol/reader.py`,
      `writer.py`. Covers D1–D8. Phantom-ACK hazard pinned by
      `test_cfg_data_payload_full_of_magics`.
- [x] **§2.3 — `Session` over a `PacketSource`**, closed and open.
      `host/protocol/session.py`. `pull()` once per packet ever, verified under
      forced backpressure (50 packets → 266 sends, 16 go-backs).
- [x] **`Link` rebuilt on the above.** `host/protocol/link.py`. `stream.py`'s
      `Sender` survives as a shim for raw-port callers (`test_comms.py`).
- [x] **Manual jog in the Tk UI as an open session.** `host/ui/online/`. One click
      = one distance, repeat clicks blend, reversal cancels.
- [x] **§4.1 — coalesced ACKs.** `data_plane.cpp` + `SimBackend`.
- [x] **§4.4 — `RX_FIXED26` inter-byte timeout.** `FIXED26_RX_TIMEOUT_MS = 50`.
- [x] Suites: `test_reader`, `test_session`, `test_ui_jog`, `test_protocol` 19/19.
- [x] **§4.9 step 2 — validated on hardware** (RP2350 on COM8, no nodes
      attached — `getpos` reads `machinePos`, which Core 1 accumulates whether
      or not anything listens on RS485). `integrity` PASS. Stress run: 2000
      packets @ 120 sps, 229 go-backs, **3638 resends all deduped**, final
      position exact. Coalescing confirmed live at 8.00 packets per ACK frame.

- [x] **§4.2 + §4.6 — extended `STATUS_RSP`, firmware half.** Magic `0xA6` →
      `0xA7`, 9 → 30 bytes, adding `pos[4]`, `expectedSeq` and `queuedUs`.
      Queued time is tracked as a single-writer counter pair (`queuedUsIn` on
      Core 0 enqueue, `queuedUsOut` on Core 1 retire) rather than one shared
      total, which would be a genuine cross-core RMW race; both are resynced
      wherever the ring is flushed. Verified on hardware by raw-wire decode:
      frame shape and CRC, `pos` agreeing with text `getpos`, `queuedUs` rising
      under load and draining to exactly 0 at IDLE, `expectedSeq` tracking
      accepted packets. `bufCount × per-segment duration` matched `queuedUs` to
      the microsecond — two independently computed fields agreeing.

- [x] **§4.2 + §4.6 — Python host parses the new frame.** `packets.py`,
      `state.py` and `SimBackend` all speak v2; the new fields are surfaced on
      `MachineStatus` as `pos` / `expected_seq` / `queued_us` and **nothing
      reads them yet** — they default to `None`, which distinguishes "this
      sample came from the text plane" from a real zero. Verified against the
      Pico. `test_get_status_mirrors_get_state` was comparing whole
      `MachineStatus` objects and now compares only the text-expressible
      fields, because STATUS_RSP is a strict superset and the two are no longer
      equal by construction.

- [x] **§4.3 — binary `seqreset`** (`0xA8`, ACK(0) back). Stream start is now
      pure data plane. `Link.reset_seq()` drains its own ACK so a session never
      opens on a stale one. 0.3 ms on hardware; text alias kept for bring-up.

- [x] **§4.5 — soft abort, firmware.** Emitter returns `EmitResult` with counted
      `out[4]`; ramp lives inside the step loop; `abortRequested` /
      `RUNNING_ABORT_DECEL` / `ABORT` (`0xA9`) / `NACK_ABORTING`. Pause shares
      the ramp. Hardware: aborted mid-stream from RUNNING, landed IDLE with
      position intact and consistent with text `getpos`, ring flushed,
      `queuedUs` resynced to 0; a fresh stream afterwards ran normally, and an
      abort while idle did not wedge the ingest barrier.

### Next

- [ ] **Un-stub the two ramp gaps** — both need Core 1 to read config:
      `DECEL_SPS2_X…A` should come from config rather than being four
      `#define`s seeded off `web/demo/config.json`, and `rampStepInBounds()`
      should do the real check (the `EMIT_SOFT_LIMIT` path around it is already
      wired end to end). **Z's decel is an unverified placeholder** — no
      `maxAccel` exists for it in the config.
- [x] **Host `abort()` + jog reversal through it.** `Link.abort()` writes
      `0xA9` fire-and-forget; `_ClickJogSource.cancel()` hands the ramp to the
      Pico instead of coasting. `_draining()` now uses reported `queued_us`, so
      `busy` means "machine moving" as a reported fact. `test_ui_jog.py` passes
      **unchanged**. Verified on hardware through the host stack: RUNNING →
      abort → IDLE, ring empty, ~937 ms of queued motion discarded, position
      kept.
      Two doc claims corrected in the process (§4.5, §4.6): the host ramp and
      the wall-clock estimate both had to stay, for reasons recorded there.
- [x] **§4.7 — `MSEG_FLAG_PATH_END` retired.** Commented out in `shared.h` and
      dropped from `MSEG_FLAG_WIRE_MASK` (0x07 → 0x06), so a host still setting
      it is ignored rather than misinterpreted. The host constant is kept at
      `0x00` so existing call sites are no-ops rather than import errors.
      Unrelated to `pipeline.stages`' live planner-internal `PATH_END`.

### Next

- [ ] **Consume the remaining new field.** `pos` from `STATUS_RSP` still is not
      used — the UI poller does a separate text `getpos` every 4th pass. The parse landed but the payoff did not:
      position still comes from the text `getpos` path, and jog pacing still
      dead-reckons. Should *delete* `LEAD_S` / `_queued_s` / `_t0` from
      `_ClickJogSource`, drop the `getpos` round trip from the UI poller, and
      let `busy` mean "machine moving" as a reported fact rather than a
      host-side estimate (§2.3).
- [ ] **`web/demo/transport.js` still expects `0xA6`/9 B.** The web demo is
      broken against current firmware until it is updated — cleanly, as an
      unknown magic, which is what the magic bump bought.
- [ ] **§4.5 — soft abort.** Last, and the only item that is not nearly free:
      `emitMicroSegment` returning `EmitResult` + counted `out[4]`,
      `abortRequested`, `RUNNING_ABORT_DECEL`, `NACK_ABORTING`. Should delete
      `_decel_distance()` and the host's ramp-down branch.
- [ ] **§4.7 — delete `MSEG_FLAG_PATH_END`** and the declared-but-unimplemented
      MCFG / TILE / TOOL magics from the frozen contract.

For 4.2/4.5/4.6 the acceptance test is stated in §4.9: `test_ui_jog.py` keeps
passing **unchanged** while host code named in each section disappears. If host
code grows instead, the firmware did not take the responsibility.

### Deferred

- [ ] **TS port to `web/src/link/`.** Held until Python settles — the API was
      designed for the async model (D12), so this is a transcription, not a
      redesign. `ctx.wait()` is the one genuine divergence (§2.3).
- [ ] **§4.10 — unsolicited status push.** Consider now that 4.1 has landed.
- [ ] **§2.7 open questions** — sink backpressure, poll fairness under a
      saturating stream, `stop` latency. All want measurement, not design.

### Loose ends

- [ ] `host/diagnostics/jog_blend_ui.py` got a mechanical port only (one session
      per burst). The open-session rewrite the UI panel received is the fix.
- [ ] Dead code: the old queue-based `jog()` / `_jog_worker` / `jog_q` path in
      `host/ui/online/session.py` is no longer reachable.
- [ ] Stale docs, all pre-dating this work: `transport.js` and `wire_protocol.md`
      still describe MCFG as sent; `host/protocol/__init__.py:4` claims an MCFG
      packer that does not exist; `shared.h`'s "major axis = 1" comment is
      contradicted by `core1.cpp`.