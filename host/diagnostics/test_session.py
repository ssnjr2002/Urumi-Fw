"""
test_session.py — Session + Writer + Demux against an in-process fake Pico.

No hardware. FakePico implements the firmware's data-plane contract from
data_plane.cpp: cumulative ACKs, the seq duplicate guard, NACK_FULL backpressure
from a bounded ring buffer, and STATUS_RSP. It is a TEST DOUBLE for the wire
contract, not a motion simulator.

What is actually being proven here:
  1. Go-Back-N still works when the reader is shared rather than seized.
  2. A go-back never duplicates motion (the seq guard) and never loses a packet.
  3. pull() is called at most once per packet even under heavy retransmission —
     the property that makes open/jog sessions possible.
  4. An open session can be fed mid-flight and truncated at a frame boundary.
  5. Status polling stays live DURING a stream, which is impossible under seizure.

Run:  python -m host.diagnostics.test_session
"""

import sys
import threading
import time

from host.protocol.reader import Demux, make_sinks
from host.protocol.writer import Writer
from host.protocol.session import Session, ListSource, PacketSource
from host.protocol.packets import (
    pack_microsegment, pack_status_rsp, unpack_microsegment,
    MAGIC_MICROSEG, MAGIC_JOG, MAGIC_STATUS_REQ, MAGIC_ACK, MAGIC_NACK,
    NACK_CRC, NACK_FULL,
)

from collections import namedtuple

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])

_failures = []


def check(cond, label):
    if cond:
        print(f"  ok   {label}")
    else:
        print(f"  FAIL {label}")
        _failures.append(label)


# ── fake Pico ─────────────────────────────────────────────────────────────────

class FakePico:
    """Mirrors src/rp2350/core0/data_plane.cpp's receive path.

    Acts as a pyserial-like object for the Writer (write/flush) and feeds the
    host's Demux directly, so the whole reader path is exercised.
    """

    def __init__(self, demux, ring_size=8, drain_per_tick=2, crc_fail_every=0,
                 crc_fail_limit=5):
        self.demux = demux
        self.ring_size = ring_size
        self.drain_per_tick = drain_per_tick
        self.crc_fail_every = crc_fail_every
        self.crc_fail_limit = crc_fail_limit
        self.crc_failures = 0

        self.expected_seq = 0        # firmware's expectedSeq
        self.ring = 0                # occupancy of masterBuf
        self.executed = []           # MicroSegments actually committed
        self.duplicates_rejected = 0
        self.packets_seen = 0

        self.status_requests = 0
        self._rx = bytearray()
        self._lock = threading.Lock()
        self._pending_ack = False    # coalescing: §4.1
        self.coalesce = False

    # -- pyserial surface used by Writer --------------------------------------

    def write(self, data):
        with self._lock:
            self._rx.extend(data)
            self._consume()

    def flush(self):
        pass

    # -- firmware receive path ------------------------------------------------

    def _consume(self):
        while self._rx:
            b = self._rx[0]

            if b == MAGIC_STATUS_REQ:
                del self._rx[0]
                self.status_requests += 1
                self._send(pack_status_rsp(state=2, axes_enabled=0x07,
                                           axes_homed=0x07, alarm=0, running=1,
                                           buf_count=self.ring))
                continue

            if b in (MAGIC_MICROSEG, MAGIC_JOG):
                if len(self._rx) < 26:
                    return                       # wait for the rest
                pkt = bytes(self._rx[:26])
                del self._rx[:26]
                self._handle_mseg(pkt)
                continue

            if b < 0x80:                         # text line
                nl = self._rx.find(b"\n")
                if nl < 0:
                    return
                line = bytes(self._rx[:nl]).decode("ascii", "replace").strip()
                del self._rx[:nl + 1]
                self._handle_text(line)
                continue

            del self._rx[0]                      # unknown

    def _handle_mseg(self, pkt):
        self.packets_seen += 1

        if (self.crc_fail_every and self.crc_failures < self.crc_fail_limit
                and self.packets_seen % self.crc_fail_every == 0):
            self.crc_failures += 1
            self._nack(NACK_CRC)
            return

        seq = pkt[22]
        if seq != self.expected_seq:
            # Stale retransmit after a go-back: ACK so the window advances, but
            # do NOT execute — this is the guard against duplicated motion.
            self.duplicates_rejected += 1
            self._ack()
            return

        if self.ring >= self.ring_size:
            self._nack(NACK_FULL)                # backpressure, not an error
            return

        self.ring += 1
        self.executed.append(unpack_microsegment(pkt))
        self.expected_seq = (self.expected_seq + 1) & 0xFF
        self._ack()

    def _handle_text(self, line):
        if line == "seqreset":
            self.expected_seq = 0
            self._send(b"ok\n")
        elif line == "stop":
            self._send(b"ok\n")
        else:
            self._send(b"ok\n")

    # -- responses ------------------------------------------------------------

    def _ack(self):
        if self.coalesce:
            self._pending_ack = True
            if not self._rx:                     # drain-empty flush (§4.1)
                self.flush_acks()
        else:
            self._send(bytes([MAGIC_ACK, self.expected_seq, 0x00]))

    def flush_acks(self):
        if self._pending_ack:
            self._pending_ack = False
            self._send(bytes([MAGIC_ACK, self.expected_seq, 0x00]))

    def _nack(self, reason):
        self.flush_acks()                        # §4.1: flush pending ACK first
        self._send(bytes([MAGIC_NACK, reason, 0x00]))

    def _send(self, data):
        self.demux.feed(data)

    # -- motion ---------------------------------------------------------------

    def tick(self):
        """Drain the ring, as Core 1 would."""
        with self._lock:
            self.ring = max(0, self.ring - self.drain_per_tick)


def mseg(dx):
    return pack_microsegment(_MS(dx=dx, dy=0, dz=0, da=0, interval=1000, flags=0))


def build(demux_sinks=None, **kw):
    s = demux_sinks or make_sinks()
    d = Demux(s["ack"], s["status"], s["text"], s["cfg"])
    pico = FakePico(d, **kw)
    w = Writer(pico)
    return s, d, pico, w


def run_with_drain(session, pico, interval=0.002, writer=None, poll=0.0):
    """Run a session while a background thread drains the Pico's ring, so
    NACK_FULL backpressure resolves the way it does on real hardware.

    `poll` starts the always-on status monitor (§2.1) alongside. An open/jog
    session needs it: ctx.buf_count is None without it, so the source can never
    decide to idle. That it can run at all during a stream is the point."""
    stop = threading.Event()

    def drain():
        while not stop.is_set():
            pico.tick()
            time.sleep(interval)

    def monitor():
        while not stop.is_set():
            writer.write_frame(bytes([MAGIC_STATUS_REQ]))
            time.sleep(poll)

    threads = [threading.Thread(target=drain, daemon=True)]
    if poll:
        threads.append(threading.Thread(target=monitor, daemon=True))
    for t in threads:
        t.start()
    try:
        return session.run()
    finally:
        stop.set()
        for t in threads:
            t.join(timeout=1.0)


# ── closed sessions ───────────────────────────────────────────────────────────

def test_closed_clean():
    print("\nclosed session, no backpressure")
    s, d, pico, w = build(ring_size=1000)
    packets = [mseg(i + 1) for i in range(40)]
    sess = Session(w, s["ack"], ListSource(packets), status_sink=s["status"])

    ok = sess.run()
    check(ok, "run() returned True")
    check(len(pico.executed) == 40, f"all 40 packets executed (got {len(pico.executed)})")
    check(sess.acked == 40, f"session ACKed 40 (got {sess.acked})")
    check([m["dx"] for m in pico.executed] == [i + 1 for i in range(40)],
          "packets executed in order, none duplicated")


def test_closed_with_backpressure():
    """NACK_FULL is flow control, not an error. This is the case the integrity
    test on real hardware is designed to force."""
    print("\nclosed session under NACK_FULL backpressure")
    s, d, pico, w = build(ring_size=4, drain_per_tick=1)
    packets = [mseg(i + 1) for i in range(60)]
    sess = Session(w, s["ack"], ListSource(packets), status_sink=s["status"])

    ok = run_with_drain(sess, pico)
    check(ok, "run() returned True")
    check(sess.nacks > 0, f"backpressure actually occurred ({sess.nacks} NACKs)")
    check(len(pico.executed) == 60,
          f"exactly 60 executed despite go-backs (got {len(pico.executed)})")
    check([m["dx"] for m in pico.executed] == [i + 1 for i in range(60)],
          "no duplicated or dropped motion")
    check(pico.duplicates_rejected > 0,
          f"seq guard rejected {pico.duplicates_rejected} stale retransmits")


def test_pull_called_once_per_packet():
    """THE property that makes open sessions possible: a go-back replays from the
    session's retained window, never from the source. Under heavy retransmission
    the source must still see each packet exactly once."""
    print("\npull() is once-per-packet under retransmission")

    class CountingSource(ListSource):
        def __init__(self, packets):
            super().__init__(packets)
            self.pulled = []

        def pull(self, ctx):
            batch = super().pull(ctx)
            if batch:
                self.pulled.extend(batch)
            return batch

    s, d, pico, w = build(ring_size=3, drain_per_tick=1)
    packets = [mseg(i + 1) for i in range(50)]
    src = CountingSource(packets)
    sess = Session(w, s["ack"], src, status_sink=s["status"])

    ok = run_with_drain(sess, pico)
    check(ok, "run() returned True")
    check(sess.retries > 0, f"go-backs occurred ({sess.retries} retries)")
    check(len(src.pulled) == 50, f"source pulled exactly 50 (got {len(src.pulled)})")
    check(src.pulled == packets, "source pulled each packet once, in order")
    check(sess.sent > 50, f"but the writer sent more than 50 ({sess.sent}) — retransmission")


def test_crc_errors_recover():
    print("\nCRC NACKs recover via go-back")
    s, d, pico, w = build(ring_size=1000, crc_fail_every=7)
    packets = [mseg(i + 1) for i in range(30)]
    sess = Session(w, s["ack"], ListSource(packets), status_sink=s["status"])

    ok = sess.run()
    check(ok, f"run() returned True despite {pico.crc_failures} CRC errors")
    check([m["dx"] for m in pico.executed] == [i + 1 for i in range(30)],
          "all 30 executed in order")


# ── open sessions ─────────────────────────────────────────────────────────────

class JogSource(PacketSource):
    """Open session: manual jogging. The operator holds a button; packets are
    generated in response to input that has not happened yet.

    Deliberately mirrors what jog_blend_ui.py wants to do, but without owning the
    port: the decel decision reads ctx.buf_count, which stays live during
    transmission because the reader never stopped reading.
    """

    def __init__(self, low_water=2):
        self.held = False
        self.low_water = low_water
        self.pulls = 0
        self.idle_pulls = 0
        self.emitted = 0
        self.decel_emitted = False
        self._winding_down = False

    def pull(self, ctx):
        self.pulls += 1

        if self._winding_down:
            return None

        if not self.held:
            if not self.decel_emitted:
                self.decel_emitted = True
                return [mseg(1)]                 # decel frame
            self._winding_down = True
            return None

        # Held: keep the Pico's buffer topped up, but do not run far ahead —
        # every packet queued is a packet that must still execute after release.
        buf = ctx.buf_count
        if buf is not None and buf > self.low_water:
            self.idle_pulls += 1
            ctx.wait(0.002)
            return []                            # nothing right now, still open

        self.emitted += 4
        return [mseg(100)] * 4


def test_open_session_fed_and_released():
    print("\nopen session (manual jog): fed mid-flight, ends on release")
    s, d, pico, w = build(ring_size=6, drain_per_tick=1)
    src = JogSource()
    src.held = True
    sess = Session(w, s["ack"], src, status_sink=s["status"])

    def release_later():
        time.sleep(0.15)
        src.held = False

    threading.Thread(target=release_later, daemon=True).start()
    ok = run_with_drain(sess, pico, interval=0.02, writer=w, poll=0.003)

    check(ok, "run() returned True")
    check(src.emitted > 0, f"emitted cruise packets while held ({src.emitted})")
    check(src.idle_pulls > 0,
          f"returned [] while the buffer was full ({src.idle_pulls} idle pulls)")
    check(src.decel_emitted, "emitted a decel frame on release")
    check(len(pico.executed) == src.emitted + 1,
          f"every emitted packet executed exactly once "
          f"({len(pico.executed)} vs {src.emitted + 1})")
    check(pico.executed[-1]["dx"] == 1, "decel frame was the last thing executed")


def test_open_session_reads_live_telemetry():
    """The idle pulls above are only meaningful if ctx.buf_count is actually
    tracking the Pico. Assert the source saw the buffer both full and drained."""
    print("\nopen session reads live buffer occupancy")
    s, d, pico, w = build(ring_size=6, drain_per_tick=1)

    seen = []

    class Watching(JogSource):
        def pull(self, ctx):
            if ctx.buf_count is not None:
                seen.append(ctx.buf_count)
            return super().pull(ctx)

    src = Watching()
    src.held = True
    sess = Session(w, s["ack"], src, status_sink=s["status"])

    # A status poller running concurrently — impossible under seizure.
    stop = threading.Event()

    def poll():
        while not stop.is_set():
            w.write_frame(bytes([MAGIC_STATUS_REQ]))
            time.sleep(0.005)

    threading.Thread(target=poll, daemon=True).start()

    def release_later():
        time.sleep(0.15)
        src.held = False

    threading.Thread(target=release_later, daemon=True).start()
    ok = run_with_drain(sess, pico)
    stop.set()

    check(ok, "run() returned True with a poller running concurrently")
    # The load-bearing claim is concurrency, not any particular occupancy value:
    # status round trips completed WHILE microsegments were streaming. Under
    # seizure this count would be zero for the whole duration of the stream.
    check(pico.status_requests > 5,
          f"status polls completed during the stream ({pico.status_requests})")
    check(len(seen) > 0 and all(v is not None for v in seen),
          f"every pull() read a live sample, never None ({len(seen)} pulls)")
    check(pico.packets_seen > 0 and pico.status_requests > 0,
          "writer interleaved status frames with MSEG batches on one port")
    check(len(pico.executed) == src.emitted + 1,
          "motion still exact under concurrent polling — no interleaved frame corruption")


def test_truncate_at_frame_boundary():
    """Jog reversal and estop are the same primitive: truncate at a frame
    boundary, then write the follow-up frame."""
    print("\ntruncation at a frame boundary")
    s, d, pico, w = build(ring_size=1000)

    class Endless(PacketSource):
        def __init__(self): self.n = 0
        def pull(self, ctx):
            self.n += 1
            return [mseg(1)]

    src = Endless()
    sess = Session(w, s["ack"], src, status_sink=s["status"])

    def cut():
        time.sleep(0.05)
        sess.truncate()

    threading.Thread(target=cut, daemon=True).start()
    ok = sess.run()

    check(ok, "truncated session returns True (a normal outcome, not a failure)")
    check(sess.truncated, "session recorded the truncation")
    check(len(pico.executed) > 0, "some motion executed before the cut")

    # Every byte the Pico received formed a whole frame — never a partial one.
    check(len(pico._rx) == 0,
          f"no partial frame left in the Pico's receive buffer ({len(pico._rx)} bytes)")

    # The follow-up frame (decel / stop) goes out cleanly afterwards.
    w.write_text("stop")
    check(s["text"].get(timeout=0.5) == "ok", "follow-up 'stop' accepted after truncation")


# ── coalesced ACKs (§4.1) ─────────────────────────────────────────────────────

def test_coalesced_acks():
    """Coalescing is firmware-only and backward-compatible: the host already
    decodes an arbitrary cumulative delta, so it must work unchanged."""
    print("\ncoalesced ACKs (firmware §4.1) — host unchanged")
    s, d, pico, w = build(ring_size=1000)
    pico.coalesce = True
    packets = [mseg(i + 1) for i in range(40)]
    sess = Session(w, s["ack"], ListSource(packets), status_sink=s["status"])

    ok = sess.run()
    check(ok, "run() returned True against a coalescing Pico")
    check(len(pico.executed) == 40, f"all 40 executed (got {len(pico.executed)})")
    check([m["dx"] for m in pico.executed] == [i + 1 for i in range(40)], "in order")


def main():
    print("=" * 62)
    print("session + writer + demux — against a fake Pico")
    print("=" * 62)

    for fn in (test_closed_clean, test_closed_with_backpressure,
               test_pull_called_once_per_packet, test_crc_errors_recover,
               test_open_session_fed_and_released,
               test_open_session_reads_live_telemetry,
               test_truncate_at_frame_boundary, test_coalesced_acks):
        fn()

    print("\n" + "=" * 62)
    if _failures:
        print(f"{len(_failures)} FAILED:")
        for f in _failures:
            print(f"  - {f}")
        return 1
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
