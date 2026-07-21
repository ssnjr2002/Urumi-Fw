"""
session.py — windowed stream sessions over the demux (docs/comms_architecture.md §2.3).

Replaces stream.Sender. The behavioural change is not Go-Back-N — that logic is
carried over intact — but OWNERSHIP: a Session subscribes to the ack sink for its
span instead of seizing the port and spawning its own reader. Status polling and
text commands stay live throughout.

CLOSED vs OPEN SESSIONS
The old Sender took a list, which silently assumed the whole packet sequence was
known before the first byte went out. That is true of a job and false of manual
jogging — an operator moving the machine in real time by holding or tapping
buttons, where packets are a function of input that has not happened yet.

Both fall out of one signature once the retransmit buffer is separated from the
packet source. In Sender those were the same list, doing both jobs. Here the
Session keeps its own `_window` for retransmission and pulls from a PacketSource
for new material, which gives the property that makes open sessions tractable:

    pull() is called AT MOST ONCE PER PACKET, EVER.

A go-back replays from the retained window, never from the source. So a source
needs no idempotency and no memory of what it already produced — exactly what a
jog source cannot provide.

    closed (job)  — pull() returns slices, then None. Never returns [].
    open  (jog)   — pull() may return [] indefinitely (operator holding a button
                    with the buffer full, or released and winding down). Returns
                    None only when finished.
"""

import sys
import threading
import time

from host.protocol.reader import Ack, Nack
from host.protocol.packets import (
    stamp_seq, NACK_CRC, NACK_FULL, NACK_BAD_MAGIC, NACK_PAUSED, NACK_BAD_STATE,
    unpack_status_rsp,
)

DEFAULT_WINDOW   = 16
ACK_TIMEOUT_S    = 0.2      # per-response wait before assuming loss
STALL_TIMEOUT_S  = 3.0      # total silence before fatal abort
BACKPRESSURE_S   = 0.05     # wait for the ring buffer to drain on NACK_FULL
MAX_CRC_ERRORS   = 20
IDLE_WAIT_S      = 0.02     # bound on an open source's idle wait


# ── what a source sees ────────────────────────────────────────────────────────

class StreamContext:
    """Handed to PacketSource.pull(). Everything a source needs to decide what to
    emit next, without owning the port or issuing a round trip.

    `status` is the key one: it is the live sample from the global status sink,
    which keeps updating DURING transmission because the reader never stopped
    reading. Under the old model buffer occupancy could only be polled between
    bursts, so a jog's blend-vs-decel decision was made on a sample stale by the
    whole duration of the send just completed.
    """

    __slots__ = ("room", "in_flight", "emitted", "acked", "_status_sink", "_session")

    def __init__(self, session, status_sink):
        self._session = session
        self._status_sink = status_sink
        self.room = 0
        self.in_flight = 0
        self.emitted = 0
        self.acked = 0

    @property
    def status(self):
        """Latest status snapshot as a dict, or None if none has arrived. A field
        read, not a round trip."""
        raw = self._status_sink.value if self._status_sink else None
        if raw is None:
            return None
        try:
            return unpack_status_rsp(raw)
        except ValueError:
            return None

    @property
    def buf_count(self):
        """MicroSegments queued on the Pico, or None. This is what a jog source
        watches to decide when to blend or wind down."""
        st = self.status
        return None if st is None else st.get("buf_count")

    @property
    def queued_sample(self):
        """(queued_us, stamp, arrival_time) — or (None, 0, 0.0).

        For a source that wants to EXTRAPOLATE between polls rather than just
        read the latest figure: `stamp` says whether this is a sample it has
        already accounted for, `arrival_time` says how old it is. Reading
        `queued_us` repeatedly cannot distinguish a fresh sample from a stale
        one, which is how an open-loop local estimate ends up drifting.
        """
        if self._status_sink is None:
            return None, 0, 0.0
        raw, stamp, at = self._status_sink.sample
        if raw is None:
            return None, stamp, at
        try:
            return unpack_status_rsp(raw).get("queued_us"), stamp, at
        except ValueError:
            return None, stamp, at

    @property
    def queued_us(self):
        """Queued MOTION TIME on the Pico in microseconds, or None (§4.6).

        What a jog source should actually pace against. `buf_count` counts
        segments, and segments differ in duration by orders of magnitude, so a
        count says nothing about how far ahead of the machine we are. Reported
        by the Pico, so it needs no wall-clock dead reckoning on this side.
        """
        st = self.status
        return None if st is None else st.get("queued_us")

    def wait(self, timeout=IDLE_WAIT_S):
        """Bounded wait for an open source with nothing to emit.

        An open source returning [] with an empty window leaves the loop nothing
        to await — the ack sink is empty precisely because nothing is in flight.
        Without this, the loop spins. Sources should override the wait target by
        supplying their own (e.g. blocking on an intent queue), so jogging does
        not gain a latency floor equal to a poll interval; this default is the
        fallback. It is also the one place the Python and TS implementations
        genuinely diverge — condition variable vs. promise race.
        """
        self._session._abort.wait(timeout)


class PacketSource:
    """Interface. pull() returns:
         None  — finished (closed: exhausted; open: winding down)
         []    — nothing right now, still open
         [pkt] — emit these
    """

    def pull(self, ctx):
        raise NotImplementedError


class ListSource(PacketSource):
    """Closed session: the whole sequence is known up front."""

    def __init__(self, packets):
        self._packets = list(packets)
        self._i = 0

    def pull(self, ctx):
        if self._i >= len(self._packets):
            return None
        take = max(1, ctx.room)
        chunk = self._packets[self._i:self._i + take]
        self._i += len(chunk)
        return chunk

    def __len__(self):
        return len(self._packets)


# ── the session ───────────────────────────────────────────────────────────────

class Session:
    """One windowed stream, subscribed to the ack sink for its span.

    Go-Back-N semantics are unchanged from Sender: the Pico processes packets in
    order and replies with a cumulative ACK ("I expect seq N next", meaning
    everything below N was accepted). On any NACK we back off and resend from
    `base`; the rolling seq in packet byte [22] makes replays idempotent on the
    firmware side, so a go-back cannot duplicate motion.
    """

    def __init__(self, writer, ack_sink, source, status_sink=None,
                 window=DEFAULT_WINDOW, verbose=False):
        self.writer = writer
        self.ack_sink = ack_sink
        self.source = source
        self.window = window
        self.verbose = verbose

        self._ctx = StreamContext(self, status_sink)
        self._abort = threading.Event()

        # Retransmit buffer: packets emitted but not yet cumulatively ACKed.
        # Separate from the source — this is what makes pull() once-per-packet.
        self._window = []        # stamped packets, index 0 == `base`
        self._base = 0           # count of packets confirmed accepted
        self._next = 0           # count of packets handed to the writer
        self._emitted = 0        # count of packets pulled from the source
        self._seq = 0            # rolling 8-bit wire seq, stamped at emit time
        self._done = False       # source returned None

        self.sent = 0
        self.acked = 0
        self.retries = 0
        self.nacks = 0
        self.truncated = False

    # -- external control -----------------------------------------------------

    def truncate(self):
        """End the session at the next frame boundary. Thread-safe — called from
        the UI thread on a jog reversal, or from an estop path.

        This is D13, and it is the SAME primitive for both: smooth jog cancel is
        frame-boundary truncation followed by a decel frame; estop is
        frame-boundary truncation followed by "stop". Whatever the source wanted
        next is discarded. It does not touch the writer, so it cannot corrupt a
        frame already in progress.
        """
        self.truncated = True
        self._abort.set()

    # -- main loop ------------------------------------------------------------

    def run(self):
        """Drive the stream to completion. Returns True on success, False on a
        fatal error. A truncated session returns True — truncation is a normal
        outcome, not a failure."""
        last_progress = time.monotonic()
        crc_errors = 0

        while True:
            if self._abort.is_set():
                break

            # Done only when the source is finished AND every packet it produced
            # has been confirmed. The test is `_emitted`, not `_next`: after a
            # go-back `_next` rewinds to `_base` while the retained window still
            # holds pulled-but-unsent packets, so testing `_next` here strands
            # them and exits reporting success.
            if self._done and self._base >= self._emitted:
                break

            self._fill_window()

            # An open source with nothing in flight and nothing to send: wait for
            # input rather than spinning. Not a stall — the operator simply is
            # not pressing anything.
            if self._next <= self._base:
                if self._done:
                    break
                self._ctx.wait()
                continue

            resp = self.ack_sink.get(timeout=ACK_TIMEOUT_S)

            if resp is None:                       # silence
                if time.monotonic() - last_progress > STALL_TIMEOUT_S:
                    print(f"FATAL: stalled — no response for {STALL_TIMEOUT_S}s "
                          f"(acked {self.acked}).", file=sys.stderr)
                    return False
                self._go_back(0x00, 0)
                continue

            if isinstance(resp, Ack):
                if self._apply_ack(resp.expected_seq):
                    last_progress = time.monotonic()
                continue

            # NACK
            self.nacks += 1
            r = resp.reason
            if r == NACK_BAD_MAGIC:
                print("FATAL: Pico reported bad magic — aborting.", file=sys.stderr)
                return False
            if r in (NACK_PAUSED, NACK_BAD_STATE):
                print(f"FATAL: stream rejected (reason 0x{r:02X}) — wrong machine "
                      f"state.", file=sys.stderr)
                return False
            if r == NACK_CRC:
                crc_errors += 1
                if crc_errors > MAX_CRC_ERRORS:
                    print(f"FATAL: {crc_errors} CRC errors — aborting.", file=sys.stderr)
                    return False
                self._go_back(r, 0)
            else:                                   # NACK_FULL — backpressure
                self._go_back(r, BACKPRESSURE_S)
            last_progress = time.monotonic()

        return True

    # -- window management ----------------------------------------------------

    def _fill_window(self):
        """Pull from the source as needed and hand whole frames to the writer."""
        room = self.window - (self._next - self._base)
        if room <= 0:
            return

        # Retransmission first: these are already in _window, never re-pulled.
        pending = self._window[self._next - self._base:]

        if not pending and not self._done:
            self._ctx.room = room
            self._ctx.in_flight = self._next - self._base
            self._ctx.emitted = self._emitted
            self._ctx.acked = self.acked

            batch = self.source.pull(self._ctx)
            if batch is None:
                self._done = True
                return
            for pkt in batch:
                self._window.append(stamp_seq(pkt, self._seq))
                self._seq = (self._seq + 1) & 0xFF
                self._emitted += 1
            pending = self._window[self._next - self._base:]

        if not pending:
            return

        written = self.writer.write_batch(pending[:room], abort=self._abort)
        self._next += written
        self.sent += written
        if self.verbose and written:
            print(f"  SEND {written} frame(s) [next={self._next}]", file=sys.stderr)

    def _apply_ack(self, expected_seq):
        """Advance on a cumulative ACK. The delta is computed in 8-bit rolling
        seq space and CLAMPED to the in-flight window, so a duplicate ACK
        (delta 0) or a stale/wrapped value can neither stall nor over-advance."""
        # base's wire seq is the seq stamped on the packet at window index 0:
        # _seq is the next seq to stamp, and (_emitted - _base) packets have been
        # stamped but not yet confirmed.
        base_seq = (self._seq - (self._emitted - self._base)) & 0xFF
        delta = (expected_seq - base_seq) & 0xFF
        if 0 < delta <= self._next - self._base:
            self._base += delta
            self.acked += delta
            del self._window[:delta]              # release confirmed packets
            return True
        return False

    def _go_back(self, reason, backoff):
        """Rewind to base for resend. Stale cumulative ACKs still in flight need
        no draining — each advances base by 0 or by a real accepted amount, so
        they can neither stall nor corrupt the window."""
        if backoff:
            time.sleep(backoff)
        self._next = self._base
        self.retries += 1
        if self.verbose:
            print(f"  GO-BACK to {self._base} (reason 0x{reason:02X})", file=sys.stderr)

    def report(self):
        print(f"\n{'-' * 50}", file=sys.stderr)
        print(f"Emitted : {self._emitted}", file=sys.stderr)
        print(f"Sent    : {self.sent}", file=sys.stderr)
        print(f"ACKed   : {self.acked}", file=sys.stderr)
        print(f"NACKs   : {self.nacks}", file=sys.stderr)
        print(f"Retries : {self.retries}", file=sys.stderr)
        if self.truncated:
            print(f"Truncated at frame boundary", file=sys.stderr)
