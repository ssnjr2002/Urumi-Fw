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

sys.path.insert(0, os.path.dirname(__file__))
from serialise import (
    validate_packet, unpack_microsegment,
    MAGIC_ACK, MAGIC_NACK,
    NACK_CRC, NACK_FULL, NACK_BAD_MAGIC,
)

# ── constants ─────────────────────────────────────────────────────────────────

DEFAULT_WINDOW   = 16       # max in-flight unACKed packets
ACK_TIMEOUT_S    = 0.2      # per-response wait before assuming loss
STALL_TIMEOUT_S  = 3.0      # total silence before fatal abort
BACKPRESSURE_S   = 0.05     # wait for buffer to drain on NACK_FULL
SETTLE_S         = 0.005    # absorb in-transit stale responses after go-back
MAX_CRC_ERRORS   = 20       # fatal only after this many CRC failures


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

        # Stats
        self.sent       = 0
        self.acked      = 0
        self.retries    = 0
        self.nacks      = 0

    def _send_packet(self, packet):
        self.ser.write(packet)
        self.ser.flush()

    def _flush_responses(self):
        """Discard all queued responses. Used on go-back: every outstanding
        response is for a packet we are about to resend, so it is stale."""
        try:
            while True:
                self.ack_queue.get_nowait()
        except queue.Empty:
            pass

    def send_stream(self, packets):
        """
        Send a list of pre-validated packets using Go-Back-N.

        The Pico processes packets strictly in order and replies one ACK/NACK
        per packet. When its ring buffer fills it NACKs (NACK_FULL) — this is
        flow control, NOT an error. Because rejection is in-order, the accepted
        packets always form a prefix; on any NACK we discard stale responses,
        back off, and resend from `base`. This preserves ordering (critical for
        motion) and never buffers duplicates.

        Returns True on success, False on fatal error.
        """
        pending    = list(packets)
        n          = len(pending)
        base       = 0          # oldest unconfirmed packet
        next_send  = 0          # next packet to transmit
        crc_errors = 0
        last_progress = time.monotonic()

        def go_back(reason, settle):
            """Discard stale responses, back off, rewind to base."""
            nonlocal next_send
            self._flush_responses()
            time.sleep(settle)
            self._flush_responses()   # absorb in-transit stale responses
            next_send = base
            self.retries += 1
            if self.verbose:
                print(f"  GO-BACK to {base} (reason 0x{reason:02X})", file=sys.stderr)

        while base < n:
            # Fill the window
            while next_send < n and (next_send - base) < self.window:
                self._send_packet(pending[next_send])
                self.sent += 1
                if self.verbose:
                    print(f"  SEND [{next_send+1}/{n}]", file=sys.stderr)
                next_send += 1

            # Wait for the next in-order response (corresponds to packet `base`)
            try:
                rtype, val = self.ack_queue.get(timeout=ACK_TIMEOUT_S)
            except queue.Empty:
                if time.monotonic() - last_progress > STALL_TIMEOUT_S:
                    print(f"FATAL: stalled — no response for {STALL_TIMEOUT_S}s "
                          f"(ACKed {self.acked}/{n}).", file=sys.stderr)
                    return False
                go_back(0x00, SETTLE_S)   # assume loss, resend from base
                continue

            if rtype == 'ACK':
                base += 1
                self.acked += 1
                last_progress = time.monotonic()
                continue

            # NACK
            self.nacks += 1
            if val == NACK_BAD_MAGIC:
                print("FATAL: Pico reported bad magic — aborting.", file=sys.stderr)
                return False
            if val == NACK_CRC:
                crc_errors += 1
                if crc_errors > MAX_CRC_ERRORS:
                    print(f"FATAL: {crc_errors} CRC errors — aborting.", file=sys.stderr)
                    return False
                go_back(val, SETTLE_S)
            else:  # NACK_FULL — backpressure, not an error
                go_back(val, BACKPRESSURE_S)
            last_progress = time.monotonic()

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
