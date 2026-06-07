"""
sender.py — windowed ACK/NACK serial sender for MicroSegment streams

Reads a length-prefixed binary stream (written by svg_to_packets.py), validates
every packet before sending, then streams to the Pico over USB CDC with a
sliding-window ACK/NACK protocol.

Usage:
  python sender.py --port COM3 --in job.bin
  python sender.py --port /dev/ttyUSB0 --in job.bin --window 32 --verbose

Protocol:
  Host sends 26-byte MicroSegment packets.
  Pico replies 3 bytes per packet:
    ACK:  [0xAA] [seq_lo] [seq_hi]
    NACK: [0xBB] [reason] [0x00]
      0x01 = CRC error   → resend packet
      0x02 = buffer full → wait and resend
      0x03 = bad magic   → abort (bug in sender)
"""

import sys, os, argparse, struct, time, threading, queue
from collections import deque

sys.path.insert(0, os.path.dirname(__file__))
from serialise import (
    validate_packet, unpack_microsegment,
    MAGIC_ACK, MAGIC_NACK,
    NACK_CRC, NACK_FULL, NACK_BAD_MAGIC,
)

# ── constants ─────────────────────────────────────────────────────────────────

DEFAULT_WINDOW   = 16       # max in-flight unACKed packets
ACK_TIMEOUT_S    = 0.1      # seconds before retransmit
BACKPRESSURE_S   = 0.01     # wait before retrying on NACK_FULL
MAX_RETRIES      = 5        # abort after this many consecutive failures


# ── framing reader (mirrors verify_packets) ───────────────────────────────────

def read_packets(src):
    while True:
        header = src.read(2)
        if not header:
            break
        if len(header) < 2:
            break
        (length,) = struct.unpack("<H", header)
        data = src.read(length)
        if len(data) < length:
            break
        yield data


# ── ACK/NACK reader thread ────────────────────────────────────────────────────

def _ack_reader(ser, ack_queue, stop_event):
    """
    Runs in a background thread. Reads 3-byte ACK/NACK responses from the
    Pico and pushes them onto ack_queue as (type, seq_or_reason) tuples.
    """
    buf = bytearray()
    while not stop_event.is_set():
        try:
            byte = ser.read(1)
        except Exception:
            break
        if not byte:
            continue
        buf += byte
        if len(buf) < 3:
            continue
        b0, b1, b2 = buf[0], buf[1], buf[2]
        buf = bytearray()
        if b0 == MAGIC_ACK:
            seq = b1 | (b2 << 8)
            ack_queue.put(('ACK', seq))
        elif b0 == MAGIC_NACK:
            ack_queue.put(('NACK', b1))
        else:
            # Unexpected byte — could be a text response from the Pico.
            # Swallow and continue; text responses only appear when idle.
            pass


# ── sender ────────────────────────────────────────────────────────────────────

class Sender:
    def __init__(self, ser, window=DEFAULT_WINDOW, verbose=False):
        self.ser      = ser
        self.window   = window
        self.verbose  = verbose

        self.ack_queue  = queue.Queue()
        self.stop_event = threading.Event()
        self._thread    = threading.Thread(
            target=_ack_reader,
            args=(ser, self.ack_queue, self.stop_event),
            daemon=True,
        )
        self._thread.start()

        # Sliding window state
        self._in_flight = deque()   # deque of (seq, packet, send_time)
        self._seq       = 0         # rolling packet counter (for logging)

        # Stats
        self.sent       = 0
        self.acked      = 0
        self.retries    = 0
        self.nacks      = 0

    def _send_packet(self, packet):
        self.ser.write(packet)
        self.ser.flush()

    def _drain_acks(self, block=False, timeout=ACK_TIMEOUT_S):
        """Process all available ACKs/NACKs. Returns list of NACK reasons."""
        nack_reasons = []
        try:
            resp_type, value = self.ack_queue.get(block=block, timeout=timeout)
            if resp_type == 'ACK':
                # Remove from in-flight — ACK confirms oldest in-flight packet
                if self._in_flight:
                    self._in_flight.popleft()
                    self.acked += 1
                    if self.verbose:
                        print(f"  ACK  seq={value}", file=sys.stderr)
            elif resp_type == 'NACK':
                nack_reasons.append(value)
                self.nacks += 1
                if self.verbose:
                    print(f"  NACK reason=0x{value:02X}", file=sys.stderr)
        except queue.Empty:
            pass

        # Drain any additional immediately available responses
        while True:
            try:
                resp_type, value = self.ack_queue.get_nowait()
                if resp_type == 'ACK':
                    if self._in_flight:
                        self._in_flight.popleft()
                        self.acked += 1
                elif resp_type == 'NACK':
                    nack_reasons.append(value)
                    self.nacks += 1
            except queue.Empty:
                break

        return nack_reasons

    def send_stream(self, packets):
        """
        Send a list of pre-validated packets with sliding window ACK/NACK.
        Returns True on success, False on fatal error.
        """
        pending   = list(packets)
        idx       = 0
        retries   = 0

        while idx < len(pending) or self._in_flight:
            # Fill the window
            while idx < len(pending) and len(self._in_flight) < self.window:
                pkt = pending[idx]
                self._send_packet(pkt)
                self._in_flight.append((self._seq, pkt, time.monotonic()))
                if self.verbose:
                    print(f"  SEND [{idx+1}/{len(pending)}] seq={self._seq}",
                          file=sys.stderr)
                self._seq += 1
                self.sent += 1
                idx += 1

            # Wait for ACKs
            block   = len(self._in_flight) >= self.window or idx >= len(pending)
            nacks   = self._drain_acks(block=block)

            for reason in nacks:
                if reason == NACK_BAD_MAGIC:
                    print("FATAL: Pico reported bad magic — aborting.", file=sys.stderr)
                    return False

                if reason == NACK_FULL:
                    time.sleep(BACKPRESSURE_S)

                # Resend oldest in-flight packet
                if self._in_flight:
                    seq, pkt, _ = self._in_flight[0]
                    self._in_flight[0] = (seq, pkt, time.monotonic())
                    self._send_packet(pkt)
                    self.retries += 1
                    retries += 1
                    if self.verbose:
                        print(f"  RETRY seq={seq} reason=0x{reason:02X}",
                              file=sys.stderr)

                if retries >= MAX_RETRIES:
                    print(f"FATAL: {MAX_RETRIES} consecutive failures — aborting.",
                          file=sys.stderr)
                    return False

            # Reset retry counter on progress
            if self.acked > 0:
                retries = 0

            # Timeout check on oldest in-flight packet
            if self._in_flight:
                seq, pkt, t_sent = self._in_flight[0]
                if time.monotonic() - t_sent > ACK_TIMEOUT_S:
                    self._send_packet(pkt)
                    self._in_flight[0] = (seq, pkt, time.monotonic())
                    self.retries += 1
                    retries += 1
                    if self.verbose:
                        print(f"  TIMEOUT retransmit seq={seq}", file=sys.stderr)

        return True

    def stop(self):
        self.stop_event.set()
        self._thread.join(timeout=1.0)

    def report(self):
        print(f"\n{'-'*50}", file=sys.stderr)
        print(f"Sent    : {self.sent}", file=sys.stderr)
        print(f"ACKed   : {self.acked}", file=sys.stderr)
        print(f"NACKs   : {self.nacks}", file=sys.stderr)
        print(f"Retries : {self.retries}", file=sys.stderr)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Send a MicroSegment binary stream to the Pico over USB CDC"
    )
    parser.add_argument("--port",    required=True,
                        help="Serial port (e.g. COM3 or /dev/ttyUSB0)")
    parser.add_argument("--in",      dest="infile", required=True,
                        help="Binary stream file produced by svg_to_packets.py")
    parser.add_argument("--baud",    type=int, default=115200)
    parser.add_argument("--window",  type=int, default=DEFAULT_WINDOW,
                        help=f"Sliding window size (default {DEFAULT_WINDOW})")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    try:
        import serial
    except ImportError:
        print("pyserial not installed — pip install pyserial", file=sys.stderr)
        sys.exit(1)

    # Load and validate all packets before opening serial
    print(f"Loading {args.infile}…", file=sys.stderr)
    with open(args.infile, "rb") as f:
        raw_packets = list(read_packets(f))

    print(f"Validating {len(raw_packets)} packets…", file=sys.stderr)
    valid = []
    failed = 0
    for i, pkt in enumerate(raw_packets):
        ok, reason = validate_packet(pkt)
        if ok:
            valid.append(pkt)
        else:
            print(f"  [{i}] INVALID — {reason}", file=sys.stderr)
            failed += 1

    if failed:
        print(f"ABORT: {failed} invalid packets — fix the stream before sending.",
              file=sys.stderr)
        sys.exit(1)

    print(f"All {len(valid)} packets valid. Opening {args.port}…", file=sys.stderr)

    with serial.Serial(args.port, args.baud, timeout=0.05) as ser:
        sender = Sender(ser, window=args.window, verbose=args.verbose)
        try:
            ok = sender.send_stream(valid)
        except KeyboardInterrupt:
            print("\nInterrupted.", file=sys.stderr)
            ok = False
        finally:
            sender.stop()
            sender.report()

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
