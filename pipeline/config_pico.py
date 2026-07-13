"""
pipeline/config_pico.py — host-side config blob tool for the RP2350.

Converts JSON ↔ msgpack and implements the CFG_SET / CFG_GET wire protocol
(docs/config_storage.md §5).  Run with --help for usage.

Dependencies:
    pip install pyserial msgpack

Usage examples:
    # Store web/demo/config.json on the Pico
    python -m pipeline.config_pico store --port COM8 web/demo/config.json

    # Read the active blob back and print as JSON
    python -m pipeline.config_pico get --port COM8

    # Read back and save to a file
    python -m pipeline.config_pico get --port COM8 --out recovered.json

    # Round-trip self-test (store, get, compare)
    python -m pipeline.config_pico test --port COM8 web/demo/config.json
"""

import argparse
import json
import struct
import sys
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial not installed — run: pip install pyserial")

try:
    import msgpack
except ImportError:
    sys.exit("msgpack not installed — run: pip install msgpack")

# ── wire constants (must match shared.h) ──────────────────────────────────────

CFG_SET_MAGIC  = 0xB0
CFG_GET_MAGIC  = 0xB1
CFG_RDY        = 0xB2
CFG_ACK        = 0xB3
CFG_NACK       = 0xB4
CFG_DATA       = 0xB5
CFG_MAX_BYTES  = 32768

NACK_REASON = {
    0x01: "CRC mismatch",
    0x02: "blob too big (or zero length)",
    0x03: "machine not IDLE/ALARM",
    0x04: "flash readback verify failed",
    0x05: "transfer timeout (inter-byte stall)",
}

# ── serial helpers ─────────────────────────────────────────────────────────────

def open_port(port: str, baud: int = 115200) -> serial.Serial:
    return serial.Serial(port, baud, timeout=5.0)


def read_exactly(ser: serial.Serial, n: int) -> bytes:
    buf = b""
    deadline = time.monotonic() + 5.0
    while len(buf) < n:
        if time.monotonic() > deadline:
            raise TimeoutError(f"read_exactly: wanted {n} bytes, got {len(buf)}")
        chunk = ser.read(n - len(buf))
        if chunk:
            buf += chunk
    return buf


def expect_byte(ser: serial.Serial, expected: int, label: str) -> None:
    got = read_exactly(ser, 1)[0]
    if got != expected:
        raise RuntimeError(
            f"{label}: expected 0x{expected:02X}, got 0x{got:02X}"
        )

# ── JSON ↔ msgpack conversion ──────────────────────────────────────────────────

def json_to_msgpack(json_path: str) -> bytes:
    with open(json_path, "r", encoding="utf-8") as f:
        obj = json.load(f)
    return msgpack.packb(obj, use_bin_type=True)


def msgpack_to_json(blob: bytes) -> object:
    return msgpack.unpackb(blob, raw=False, strict_map_key=False)

# ── CFG_SET — store a blob ─────────────────────────────────────────────────────

def cfg_set(ser: serial.Serial, payload: bytes) -> None:
    """
    Two-phase CFG_SET:
      Phase 1 — send 9-byte header (magic + length u32 LE + crc32 u32 LE),
                 wait for CFG_RDY (0xB2) or CFG_NACK.
      Phase 2 — stream payload bytes, wait for CFG_ACK (0xB3) or CFG_NACK.
    Raises RuntimeError on any NACK or unexpected response.
    """
    if len(payload) == 0 or len(payload) > CFG_MAX_BYTES:
        raise ValueError(f"payload length {len(payload)} out of range 1..{CFG_MAX_BYTES}")

    crc = zlib.crc32(payload) & 0xFFFFFFFF

    # ── phase 1: header ────────────────────────────────────────────────────────
    header = bytes([CFG_SET_MAGIC]) + struct.pack("<II", len(payload), crc)
    ser.write(header)
    ser.flush()

    print(f"  → sent header: {len(payload)} bytes, CRC32=0x{crc:08X}")

    rdy = read_exactly(ser, 1)[0]
    if rdy == CFG_NACK:
        reason_byte = read_exactly(ser, 1)[0]
        reason = NACK_REASON.get(reason_byte, f"unknown reason 0x{reason_byte:02X}")
        raise RuntimeError(f"CFG_NACK in phase 1: {reason}")
    if rdy != CFG_RDY:
        raise RuntimeError(f"phase 1: expected CFG_RDY (0x{CFG_RDY:02X}), got 0x{rdy:02X}")

    print("  ← CFG_RDY — streaming payload …")

    # ── phase 2: payload ───────────────────────────────────────────────────────
    CHUNK = 256
    sent = 0
    while sent < len(payload):
        chunk = payload[sent:sent + CHUNK]
        ser.write(chunk)
        sent += len(chunk)
        pct = sent * 100 // len(payload)
        print(f"\r  → {sent}/{len(payload)} bytes ({pct}%)", end="", flush=True)
    ser.flush()
    print()

    ack = read_exactly(ser, 1)[0]
    if ack == CFG_NACK:
        reason_byte = read_exactly(ser, 1)[0]
        reason = NACK_REASON.get(reason_byte, f"unknown reason 0x{reason_byte:02X}")
        raise RuntimeError(f"CFG_NACK in phase 2: {reason}")
    if ack != CFG_ACK:
        raise RuntimeError(f"phase 2: expected CFG_ACK (0x{CFG_ACK:02X}), got 0x{ack:02X}")

    print("  ← CFG_ACK — blob committed")

# ── CFG_GET — read the active blob ────────────────────────────────────────────

def cfg_get(ser: serial.Serial) -> bytes | None:
    """
    Send CFG_GET_MAGIC, parse CFG_DATA response.
    Returns the raw msgpack payload, or None if no config is stored.
    """
    ser.write(bytes([CFG_GET_MAGIC]))
    ser.flush()

    expect_byte(ser, CFG_DATA, "CFG_GET response magic")

    hdr = read_exactly(ser, 8)
    length, crc = struct.unpack("<II", hdr)

    if length == 0:
        print("  ← CFG_DATA: no config stored (length=0)")
        return None

    print(f"  ← CFG_DATA: {length} bytes, CRC32=0x{crc:08X}")
    payload = read_exactly(ser, length)

    actual_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if actual_crc != crc:
        raise RuntimeError(
            f"CRC mismatch on received blob: got 0x{actual_crc:08X}, expected 0x{crc:08X}"
        )
    print(f"  CRC OK")
    return payload

# ── subcommands ───────────────────────────────────────────────────────────────

def cmd_store(args):
    payload = json_to_msgpack(args.json_file)
    print(f"Packed {args.json_file} → {len(payload)} bytes msgpack")
    with open_port(args.port, args.baud) as ser:
        time.sleep(0.1)          # let the CDC enumerate
        ser.reset_input_buffer()
        cfg_set(ser, payload)
    print("Done.")


def cmd_get(args):
    with open_port(args.port, args.baud) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()
        blob = cfg_get(ser)

    if blob is None:
        print("No config stored.")
        return

    obj = msgpack_to_json(blob)
    text = json.dumps(obj, indent=2)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
        print(f"Saved to {args.out}")
    else:
        print(text)


def make_max_blob() -> bytes:
    """
    Generate a msgpack blob of exactly CFG_MAX_BYTES by measuring the actual
    struct overhead rather than hardcoding it (overhead shifts with msgpack bin
    format tier: bin8 at <256 bytes, bin16 at <65536, bin32 above).
    """
    probe = msgpack.packb({"data": b""}, use_bin_type=True)
    overhead = len(probe)  # overhead with zero-length payload
    fill_size = CFG_MAX_BYTES - overhead
    blob = msgpack.packb({"data": bytes([0xA5] * fill_size)}, use_bin_type=True)
    # overhead may grow when the payload crosses a bin-format tier boundary
    while len(blob) > CFG_MAX_BYTES:
        fill_size -= 1
        blob = msgpack.packb({"data": bytes([0xA5] * fill_size)}, use_bin_type=True)
    return blob


def cmd_stress(args):
    """Generate a max-size (~32 KB) msgpack blob and round-trip it."""
    payload = make_max_blob()
    print(f"Generated max blob: {len(payload)} bytes (limit {CFG_MAX_BYTES})")
    assert len(payload) <= CFG_MAX_BYTES, "blob exceeds CFG_MAX_BYTES — fix OVERHEAD"

    crc_orig = zlib.crc32(payload) & 0xFFFFFFFF

    with open_port(args.port, args.baud) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        print("[1/3] Storing …")
        cfg_set(ser, payload)

        print("[2/3] Reading back …")
        blob = cfg_get(ser)

    if blob is None:
        print("FAIL: CFG_GET returned empty after a successful CFG_SET")
        sys.exit(1)

    print("[3/3] Comparing …")
    crc_recv = zlib.crc32(blob) & 0xFFFFFFFF
    if crc_orig == crc_recv and blob == payload:
        print(f"PASS: {len(payload)}-byte blob round-trips correctly")
    else:
        print(f"FAIL: CRC orig=0x{crc_orig:08X} recv=0x{crc_recv:08X} match={blob==payload}")
        sys.exit(1)


def cmd_test(args):
    """Round-trip: store, get, compare decoded objects."""
    with open(args.json_file, "r", encoding="utf-8") as f:
        original = json.load(f)

    payload = json_to_msgpack(args.json_file)
    print(f"[1/4] Packed {args.json_file} → {len(payload)} bytes msgpack")

    with open_port(args.port, args.baud) as ser:
        time.sleep(0.1)
        ser.reset_input_buffer()

        print("[2/4] Storing …")
        cfg_set(ser, payload)

        print("[3/4] Reading back …")
        blob = cfg_get(ser)

    if blob is None:
        print("FAIL: CFG_GET returned empty after a successful CFG_SET")
        sys.exit(1)

    recovered = msgpack_to_json(blob)
    print("[4/4] Comparing …")

    original_json  = json.dumps(original,  sort_keys=True)
    recovered_json = json.dumps(recovered, sort_keys=True)
    if original_json == recovered_json:
        print("PASS: round-trip matches")
    else:
        print("FAIL: mismatch")
        # Show a diff-friendly side by side
        orig_lines = original_json.splitlines()
        recv_lines = recovered_json.splitlines()
        import difflib
        for line in difflib.unified_diff(orig_lines, recv_lines,
                                         fromfile="original", tofile="recovered"):
            print(line)
        sys.exit(1)

# ── CLI ────────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Pico config blob tool — JSON ↔ msgpack over USB CDC"
    )
    parser.add_argument("--port", default="COM8", help="Serial port (default: COM8)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_store = sub.add_parser("store", help="Pack JSON to msgpack and store on Pico")
    p_store.add_argument("json_file", help="Path to input JSON file")
    p_store.set_defaults(func=cmd_store)

    p_get = sub.add_parser("get", help="Read active blob from Pico, print as JSON")
    p_get.add_argument("--out", metavar="FILE", help="Write JSON output to file")
    p_get.set_defaults(func=cmd_get)

    p_test = sub.add_parser("test", help="Round-trip: store JSON, get back, compare")
    p_test.add_argument("json_file", help="Path to input JSON file")
    p_test.set_defaults(func=cmd_test)

    p_stress = sub.add_parser("stress", help="Round-trip a generated ~32 KB max-size blob")
    p_stress.set_defaults(func=cmd_stress)

    args = parser.parse_args()
    try:
        args.func(args)
    except (RuntimeError, TimeoutError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
