"""
stream.py — Sender: a windowed stream over a RAW serial port.

Go-Back-N, seq stamping and the ACK loop now live in session.py; this is a thin
shim that stands up a private reader/writer/session stack on a bare pyserial
object. It exists for callers that hold a port directly rather than a Link —
principally host/diagnostics/test_comms.py, the on-hardware conformance suite.

If you have a Link, use link.stream(packets) instead. Do NOT point a Sender at
link.serial: the Link already runs a reader on that port, and two readers racing
for the same bytes will each see a random half of every reply.

Protocol (docs/wire_protocol.md):
  Host sends 26-byte MicroSegment packets.
  ACK:  [0xAA] [expectedSeq] [0x00]   cumulative: seqs below expectedSeq accepted
  NACK: [0xBB] [reason] [0x00]        0x01 CRC, 0x02 buffer full, 0x03 bad magic
"""

import struct

from host.protocol.reader import Demux, Reader, make_sinks
from host.protocol.writer import Writer
from host.protocol.session import Session, ListSource, DEFAULT_WINDOW


# ── framing reader (mirrors verify_packets) ───────────────────────────────────

def read_packets(src):
    while True:
        header = src.read(2)
        if not header or len(header) < 2:
            break
        (length,) = struct.unpack("<H", header)
        data = src.read(length)
        if len(data) < length:
            break
        yield data


# ── sender ────────────────────────────────────────────────────────────────────

class Sender:
    """Compatibility surface over Session for raw-port callers."""

    def __init__(self, ser, window=DEFAULT_WINDOW, verbose=False):
        self.ser = ser
        self.window = window
        self.verbose = verbose

        self.sinks = make_sinks()
        self.demux = Demux(self.sinks["ack"], self.sinks["status"],
                           self.sinks["text"], self.sinks["cfg"])
        self.reader = Reader(ser, self.demux).start()
        self.writer = Writer(ser)

        self._session = None
        self.sent = self.acked = self.retries = self.nacks = 0

    def send_stream(self, packets):
        """Send a list of pre-validated packets. Returns True on success."""
        self.writer.write_text("seqreset")     # align the Pico's expectedSeq
        self.sinks["text"].get(timeout=1.0)

        self._session = Session(self.writer, self.sinks["ack"],
                                ListSource(packets), status_sink=self.sinks["status"],
                                window=self.window, verbose=self.verbose)
        try:
            return self._session.run()
        finally:
            s = self._session
            self.sent, self.acked = s.sent, s.acked
            self.retries, self.nacks = s.retries, s.nacks

    def stop(self):
        self.reader.stop()

    def report(self):
        if self._session:
            self._session.report()
