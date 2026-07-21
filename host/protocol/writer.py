"""
writer.py — the frame-granular writer (docs/comms_architecture.md D6-D8).

Demultiplexing is hard; multiplexing is easy. The writer knows what it is
sending and needs none of the reader's machinery. It has exactly one job:
ATOMICITY — never let two concerns interleave bytes within a frame.

The hazard is concrete. If a status poll writes 0xA5 midway through a 26-byte
MSEG, the firmware is in rxKind = RX_FIXED26 and takes it UNCONDITIONALLY as
packet payload — there is no dispatch check mid-packet. The MSEG then fails CRC
and the status request is silently eaten, hanging its caller to timeout.

Ownership granularity mirrors the reader's: the reader owns the port always, the
writer owns it PER FRAME, released between. That is what lets a status poll slot
between two MSEGs without seizing anything.

The writer does not read (D8). Today link._sendText writes and immediately awaits
a reply on the same call; here the caller awaits its sink instead. That
decoupling is what allows a poll to be in flight while a stream is writing.
"""

import threading


class Writer:
    """Serialises access to the port at frame granularity.

    Batching is compatible with atomicity: the atomic unit is not one packet but
    a whole number of frames. A 16-packet window write is one acquisition, one
    write, one flush — 416 bytes, still atomic, and per §1 that is where the
    throughput win lives (cost is per-transaction, not per-byte).
    """

    def __init__(self, ser, max_batch_frames=16):
        self.ser = ser
        self._lock = threading.Lock()
        self.max_batch_frames = max_batch_frames

        self.frames_written = 0
        self.batches = 0
        self.bytes_written = 0
        self.aborts = 0            # batches cut short at a frame boundary

    # -- single frame ---------------------------------------------------------

    def write_frame(self, data):
        """Write one frame atomically. Used for status polls, text lines, and
        one-off binary commands."""
        with self._lock:
            self._raw(data)
            self.frames_written += 1

    def write_text(self, line):
        """Write a control-plane text line. Does NOT read the reply — the caller
        awaits the text sink (D8)."""
        if not line.endswith("\n"):
            line += "\n"
        self.write_frame(line.encode("ascii"))

    # -- batch ----------------------------------------------------------------

    def write_batch(self, frames, abort=None):
        """Write up to max_batch_frames frames as one transfer, and return how
        many were actually written.

        `abort` is a threading.Event. It is checked BETWEEN frames only (D13):
        an urgent write waits at most one frame, never a whole batch, and we
        never abandon mid-frame on the normal path. Abandoning mid-frame would
        wedge the firmware's data plane in RX_FIXED26, where it eats every other
        plane's traffic until the inter-byte timeout fires — blinding exactly the
        telemetry you most want during an estop.

        Returning the count (rather than raising) is what lets a caller rewind
        its window precisely: unwritten frames were never sent, so they need no
        retransmission logic, just a smaller `next_send`.
        """
        if not frames:
            return 0

        with self._lock:
            # Truncation happens BEFORE the write, so the batch we commit to is
            # the batch that goes out whole.
            batch = frames[:self.max_batch_frames]
            if abort is not None and abort.is_set():
                self.aborts += 1
                return 0

            self._raw(b"".join(batch))
            self.frames_written += len(batch)
            self.batches += 1
            return len(batch)

    # -- internals ------------------------------------------------------------

    def _raw(self, data):
        self.ser.write(data)
        self.ser.flush()
        self.bytes_written += len(data)

    def stats(self):
        return {
            "frames": self.frames_written,
            "batches": self.batches,
            "bytes": self.bytes_written,
            "aborts": self.aborts,
            "avg_batch": round(self.frames_written / self.batches, 1) if self.batches else 0,
        }
