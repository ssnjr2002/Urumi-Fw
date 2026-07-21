"""
reader.py — the demultiplexing reader (docs/comms_architecture.md D1-D5).

One reader owns the port for the connection's lifetime, classifies every inbound
byte, and fans frames out to typed sinks. Nothing else reads the port; sessions
and pollers subscribe rather than seize. This is what allows a status poll to be
in flight while a stream is writing.

Two pieces, deliberately separate:

  Demux   — a pure state machine. feed(bytes) -> dispatch to sinks. No I/O, so
            it is testable by handing it byte strings, including adversarial
            ones (split frames, CFG_DATA payloads full of magic bytes).
  Reader  — the thread that pumps a port into Demux.feed(). Python-specific;
            the browser replaces it with an async read loop over the same Demux.

WHY A STATE MACHINE AND NOT A MAGIC SCANNER
The old scanner in stream.py._ack_reader looked for 0xAA/0xBB anywhere in the
byte soup, relying on "those bytes never occur in ASCII text". That holds for
text and breaks for CFG_DATA, whose payload is an opaque msgpack blob that can
contain 0xAA, 0xBB or 0xA6 at any offset. A scanner would emit a phantom ACK
from inside config data, advancing a session's `base` against a packet that was
never sent — blob-dependent, intermittent, and near-impossible to trace.

So: fixed-length frames are consumed blind by count, length-prefixed payloads are
consumed opaquely by count, and nothing is ever scanned for structure.
"""

import queue
import threading

from host.protocol.packets import (
    MAGIC_ACK, MAGIC_NACK, MAGIC_STATUS_RSP, STATUS_RSP_SIZE,
    MAGIC_CFG_RDY, MAGIC_CFG_ACK, MAGIC_CFG_NACK, MAGIC_CFG_DATA,
    CFG_DATA_HDR_SIZE,
)


# ── sinks ─────────────────────────────────────────────────────────────────────
# Each sink is the sole destination for one class of frame. Routing on magic is
# what removes the need to flush the port before a request (D10): a stale status
# reply can no longer be mistaken for a text reply, because it never lands in the
# same place.

class Sink:
    """Queue sink — ordered delivery, nothing dropped. Used for acks and text,
    where every frame carries information the consumer needs."""

    def __init__(self, name):
        self.name = name
        self._q = queue.Queue()

    def put(self, item):
        self._q.put(item)

    def get(self, timeout=None):
        """Return the next item, or None on timeout."""
        try:
            return self._q.get(timeout=timeout)
        except queue.Empty:
            return None

    def clear(self):
        """Drop anything queued. NOT part of the normal path — D10 removes the
        need to flush. This exists for connection setup, where bytes from a
        previous process's session may still be in the OS buffer."""
        try:
            while True:
                self._q.get_nowait()
        except queue.Empty:
            pass

    def __len__(self):
        return self._q.qsize()


class LatestSink:
    """Latest-wins slot — for state samples, where only the newest matters and an
    old one is worthless rather than merely late (D9). A reply delayed past its
    caller's timeout is still a genuine, slightly older sample; the next poll
    corrects it. Never blocks a producer, never accumulates a backlog.

    Waiters are woken on every update, which is what lets an open jog session
    react to buffer occupancy as it changes rather than polling for it."""

    def __init__(self, name):
        self.name = name
        self._cv = threading.Condition()
        self._value = None
        self._stamp = 0          # monotonically increasing update counter

    def put(self, item):
        with self._cv:
            self._value = item
            self._stamp += 1
            self._cv.notify_all()

    @property
    def value(self):
        """The most recent sample, or None if nothing has arrived yet. Non
        blocking by design — this is a field read, not a round trip."""
        with self._cv:
            return self._value

    def wait_update(self, timeout=None, since=None):
        """Block until a sample newer than `since` arrives. Returns (value,
        stamp); value is None only if nothing ever arrived. Pass the previously
        returned stamp as `since` to avoid missing an update that landed between
        calls."""
        with self._cv:
            if since is not None and self._stamp > since:
                return self._value, self._stamp
            start = self._stamp
            self._cv.wait_for(lambda: self._stamp > start, timeout=timeout)
            return self._value, self._stamp


# ── frame types ───────────────────────────────────────────────────────────────
# Deliberately plain tuples-with-names rather than parsed objects: the demux's
# job is routing, not interpretation. Status parsing stays in state.py.

class Ack:
    __slots__ = ("expected_seq",)
    def __init__(self, expected_seq): self.expected_seq = expected_seq
    def __repr__(self): return f"Ack({self.expected_seq})"


class Nack:
    __slots__ = ("reason",)
    def __init__(self, reason): self.reason = reason
    def __repr__(self): return f"Nack(0x{self.reason:02X})"


class CfgReply:
    """kind is one of the CFG magics. `reason` is set for CFG_NACK, `payload`
    and `crc` for CFG_DATA."""
    __slots__ = ("kind", "reason", "payload", "crc")
    def __init__(self, kind, reason=None, payload=None, crc=None):
        self.kind, self.reason, self.payload, self.crc = kind, reason, payload, crc
    def __repr__(self):
        return f"CfgReply(0x{self.kind:02X}, reason={self.reason}, " \
               f"payload={None if self.payload is None else len(self.payload)}B)"


# ── the state machine ─────────────────────────────────────────────────────────

_S_IDLE        = 0
_S_FIXED       = 1   # collecting a known-length frame
_S_CFG_DATA_HDR = 2  # collecting CFG_DATA's 9-byte header
_S_CFG_DATA_PAY = 3  # collecting `length` opaque payload bytes
_S_TEXT        = 4   # collecting an ASCII line to '\n'

# Inbound fixed-length frames: magic -> total frame size including the magic.
_FIXED_SIZES = {
    MAGIC_ACK:       3,
    MAGIC_NACK:      3,
    MAGIC_STATUS_RSP: STATUS_RSP_SIZE,
    MAGIC_CFG_RDY:   1,
    MAGIC_CFG_ACK:   1,
    MAGIC_CFG_NACK:  2,
}

MAX_TEXT_LINE = 512   # guard: a runaway line must not grow without bound

# Cap on a single CFG_DATA payload. The firmware bounds this by CFG_MAX_BYTES;
# we bound it independently so a corrupted length field cannot make the reader
# sit in _S_CFG_DATA_PAY forever, swallowing every other plane's traffic.
MAX_CFG_PAYLOAD = 64 * 1024


class Demux:
    """Classify inbound bytes and route whole frames to sinks.

    Sinks are supplied by the caller so tests can pass recording doubles. All
    four are required — a missing sink is a routing hole, not a default.
    """

    def __init__(self, ack_sink, status_sink, text_sink, cfg_sink):
        self.ack = ack_sink
        self.status = status_sink
        self.text = text_sink
        self.cfg = cfg_sink

        self._state = _S_IDLE
        self._buf = bytearray()
        self._need = 0
        self._magic = 0

        # Diagnostics. `unknown_bytes` is the one to watch: a nonzero count means
        # either a firmware/host version mismatch or a genuine desync, and it is
        # the only way either becomes visible (D5).
        self.unknown_bytes = 0
        self.frames = 0
        self.text_lines = 0
        self.overruns = 0        # oversized text line or CFG payload

    # -- entry point ----------------------------------------------------------

    def feed(self, data):
        """Consume a chunk of bytes. Frame boundaries need not align with chunk
        boundaries — a frame split across two reads is the normal case at 64-byte
        USB packet quantum, so this is exercised constantly, not rarely."""
        for b in data:
            self._byte(b)

    def _byte(self, b):
        if self._state == _S_IDLE:
            self._dispatch(b)
            return

        if self._state == _S_TEXT:
            if b == 0x0A:                       # '\n' terminates
                line = self._buf.decode("ascii", errors="replace").rstrip("\r")
                self._buf.clear()
                self._state = _S_IDLE
                if line:                        # ignore blank lines
                    self.text_lines += 1
                    self.text.put(line)
            elif len(self._buf) >= MAX_TEXT_LINE:
                self.overruns += 1
                self._buf.clear()
                self._state = _S_IDLE
            else:
                self._buf.append(b)
            return

        # All remaining states are pure byte counting — no inspection of content.
        self._buf.append(b)
        if len(self._buf) < self._need:
            return

        if self._state == _S_FIXED:
            self._emit_fixed()
        elif self._state == _S_CFG_DATA_HDR:
            self._cfg_data_header()
        else:                                    # _S_CFG_DATA_PAY
            self._emit_cfg_data()

    # -- idle dispatch --------------------------------------------------------

    def _dispatch(self, b):
        size = _FIXED_SIZES.get(b)
        if size is not None:
            self._magic = b
            if size == 1:                        # complete on its own
                self.frames += 1
                self.cfg.put(CfgReply(b))
                return
            self._buf.clear()
            self._buf.append(b)
            self._need = size
            self._state = _S_FIXED
            return

        if b == MAGIC_CFG_DATA:
            self._buf.clear()
            self._buf.append(b)
            self._need = CFG_DATA_HDR_SIZE
            self._state = _S_CFG_DATA_HDR
            return

        if b < 0x80:                             # ASCII — control-plane text
            self._buf.clear()
            if b != 0x0A:                        # a bare '\n' is not a line
                self._buf.append(b)
                self._state = _S_TEXT
            return

        # Bit 7 set but not a magic we know: discard exactly one byte and stay
        # idle, so the next byte gets a fresh classification. Discarding more
        # would risk eating the start of a valid frame (D5).
        self.unknown_bytes += 1

    # -- completions ----------------------------------------------------------

    def _emit_fixed(self):
        buf, magic = self._buf, self._magic
        self._state = _S_IDLE
        self.frames += 1

        if magic == MAGIC_ACK:
            self.ack.put(Ack(buf[1]))
        elif magic == MAGIC_NACK:
            self.ack.put(Nack(buf[1]))
        elif magic == MAGIC_STATUS_RSP:
            self.status.put(bytes(buf))          # parsed by state.py downstream
        elif magic == MAGIC_CFG_NACK:
            self.cfg.put(CfgReply(magic, reason=buf[1]))
        self._buf = bytearray()

    def _cfg_data_header(self):
        buf = self._buf
        length = int.from_bytes(buf[1:5], "little")
        crc    = int.from_bytes(buf[5:9], "little")
        self._cfg_crc = crc

        if length == 0:                          # no config stored — done
            self._state = _S_IDLE
            self.frames += 1
            self._buf = bytearray()
            self.cfg.put(CfgReply(MAGIC_CFG_DATA, payload=b"", crc=crc))
            return

        if length > MAX_CFG_PAYLOAD:
            # Do not enter the payload state: we would consume `length` bytes of
            # whatever follows, blinding every other plane. Drop and resync.
            self.overruns += 1
            self._state = _S_IDLE
            self._buf = bytearray()
            return

        self._buf = bytearray()
        self._need = length
        self._state = _S_CFG_DATA_PAY

    def _emit_cfg_data(self):
        payload = bytes(self._buf)
        self._buf = bytearray()
        self._state = _S_IDLE
        self.frames += 1
        self.cfg.put(CfgReply(MAGIC_CFG_DATA, payload=payload, crc=self._cfg_crc))

    # -- diagnostics ----------------------------------------------------------

    @property
    def idle(self):
        """True when not mid-frame. Tests assert this after feeding a complete
        byte sequence: a non-idle demux means a frame was under-consumed."""
        return self._state == _S_IDLE

    def stats(self):
        return {
            "frames": self.frames,
            "text_lines": self.text_lines,
            "unknown_bytes": self.unknown_bytes,
            "overruns": self.overruns,
        }


# ── the reader thread ─────────────────────────────────────────────────────────

class Reader:
    """Pumps a serial port into a Demux for the connection's lifetime (D1).

    Bulk reads, not read(1) per byte (D4): at the 64-byte USB quantum a per-byte
    read costs one syscall per byte for no benefit, and it was a measurable part
    of why the old ack reader struggled to keep up with a saturating stream.
    """

    def __init__(self, ser, demux, chunk=4096):
        self.ser = ser
        self.demux = demux
        self.chunk = chunk
        self._stop = threading.Event()
        self._thread = None
        self.error = None        # set if the read loop dies unexpectedly

    def start(self):
        self._thread = threading.Thread(
            target=self._loop, name="demux-reader", daemon=True)
        self._thread.start()
        return self

    def _loop(self):
        while not self._stop.is_set():
            try:
                # read(1) blocks up to the port timeout, then in_waiting tells us
                # how much more arrived together; take it all in one call.
                first = self.ser.read(1)
                if not first:
                    continue
                extra = self.ser.in_waiting
                data = first + self.ser.read(extra) if extra else first
                self.demux.feed(data)
            except Exception as e:              # port closed, unplugged, etc.
                if not self._stop.is_set():
                    self.error = e
                break

    def stop(self, timeout=1.0):
        self._stop.set()
        if self._thread:
            self._thread.join(timeout=timeout)


def make_sinks():
    """The standard sink set. Status is latest-wins; the rest are queues."""
    return {
        "ack":    Sink("ack"),
        "status": LatestSink("status"),
        "text":   Sink("text"),
        "cfg":    Sink("cfg"),
    }
