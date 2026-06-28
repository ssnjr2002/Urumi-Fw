"""
serialise.py — wire-format packers for all Pico USB packet types.

Two production modes share this file:

  Host production  (this branch, microseg-host-drive):
    SVG → stages 1–6 → pack_microsegment() → Pico
    The Pico receives pre-computed step events and emits them directly.

  Local production (future branch, svg-tile-motion):
    SVG → stages 1–3 → pack_tool_config() + pack_spline_tile() → Pico
    The Pico runs stages 4–6 internally (C++ port of the Python reference).

The magic byte on the wire tells the Pico which packet type it is receiving.
Both modes can coexist on the same USB stream.

Packet formats
──────────────
MicroSegment   (26 bytes)  magic = 0xAB
  [0]      magic
  [1..4]   dx       int32 LE   X steps (signed)
  [5..8]   dy       int32 LE   Y steps (signed)
  [9..12]  dz       int32 LE   Z steps (signed, +lift / -lower)
  [13..16] da       int32 LE   A steps (signed, tangential rotation)
  [17..20] interval uint32 LE  step interval in RP2350 CPU cycles
  [21]     flags    uint8      MSEG_FLAG_* bitmask
  [22]     seq      uint8      rolling sequence number (stamped by the sender;
                               lets the Pico drop Go-Back-N retransmits of
                               packets it already accepted — see stamp_seq)
  [23..24] pad      2 bytes    zero (matches C struct alignment)
  [25]     CRC8 over bytes [0..24]

ToolConfig     (21 bytes)  magic = 0xAC
  [0]      magic
  [1..2]   seq_num  uint16 LE
  [3]      tool_type  uint8  (0=JOG 1=CUT 2=CREASE)
  [4..7]   feed_max   float32 LE  mm/s
  [8..11]  lift_kappa float32 LE  1/mm
  [12..15] lift_height float32 LE mm
  [16..19] z_feed     float32 LE  mm/s
  [20]     CRC8 over bytes [0..19]

SplineTile     (37 bytes)  magic = 0xAD
  [0]      magic
  [1..2]   seq_num  uint16 LE
  [3]      flags    uint8  (bit0=MERGE_WITH_PREV bit1=PATH_START bit2=PATH_END)
  [4..35]  control_points[4][2] float32 LE pairs (P0x P0y … P3x P3y)
  [36]     CRC8 over bytes [0..35]

ACK  (3 bytes): [0xAA] [seq_lo] [seq_hi]
NACK (3 bytes): [0xBB] [reason] [0x00]
  reason 0x01 = CRC error
  reason 0x02 = buffer full (backpressure)
  reason 0x03 = bad magic
"""

import struct
import math
from collections import namedtuple

# ── magic bytes ───────────────────────────────────────────────────────────────

MAGIC_MICROSEG   = 0xAB
MAGIC_TOOL       = 0xAC
MAGIC_SPLINE     = 0xAD

MAGIC_ACK        = 0xAA
MAGIC_NACK       = 0xBB

NACK_CRC         = 0x01
NACK_FULL        = 0x02
NACK_BAD_MAGIC   = 0x03

# ── MicroSegment flags ────────────────────────────────────────────────────────
# One byte, one namespace (see docs/wire_protocol.md). Low bits are wire/firmware
# semantics; high bits (0x08 LIFT, 0x10 JOG; defined as MICRO_* in microsegment.py)
# are host planning hints the firmware masks off.

MSEG_FLAG_NONE     = 0x00
MSEG_FLAG_PATH_END = 0x01
MSEG_FLAG_ESTOP    = 0x02
MSEG_FLAG_PAUSE    = 0x04   # sender-inserted at a tool-change boundary (single head)

# ── SplineTile flags ──────────────────────────────────────────────────────────

TILE_MERGE_WITH_PREV = 0x01
TILE_PATH_START      = 0x02
TILE_PATH_END        = 0x04

# ── tool types ────────────────────────────────────────────────────────────────

TOOL_JOG    = 0
TOOL_CUT    = 1
TOOL_CREASE = 2

# ── ToolConfig namedtuple ─────────────────────────────────────────────────────

ToolConfig = namedtuple("ToolConfig", [
    "tool_type",    # TOOL_JOG / TOOL_CUT / TOOL_CREASE
    "feed_max",     # mm/s
    "lift_kappa",   # 1/mm — curvature threshold for auto-lift
    "lift_height",  # mm
    "z_feed",       # mm/s
])

TOOL_CONFIG_DEFAULT = ToolConfig(
    tool_type=TOOL_CUT,
    feed_max=80.0,
    lift_kappa=1.5,
    lift_height=1.0,
    z_feed=10.0,
)

# ── CRC-8 (polynomial 0x8C, matches Pico firmware) ───────────────────────────

def _crc8(data: bytes) -> int:
    crc = 0x00
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x01:
                crc = (crc >> 1) ^ 0x8C
            else:
                crc >>= 1
    return crc

# ── MicroSegment packer/unpacker ──────────────────────────────────────────────

PACKET_SIZES = {
    MAGIC_MICROSEG: 26,
    MAGIC_TOOL:     21,
    MAGIC_SPLINE:   37,
}

def validate_packet(data: bytes) -> tuple:
    """
    Validate a raw packet. Returns (ok: bool, reason: str).
    Checks magic byte, expected size, and CRC.
    """
    if not data:
        return False, "empty packet"
    magic = data[0]
    if magic not in PACKET_SIZES:
        return False, f"bad magic 0x{magic:02X}"
    expected = PACKET_SIZES[magic]
    if len(data) != expected:
        return False, f"bad size {len(data)} (expected {expected})"
    if _crc8(data[:-1]) != data[-1]:
        return False, "CRC mismatch"
    return True, ""


def pack_microsegment(ms) -> bytes:
    """
    Pack a MicroSegment namedtuple (from stage 6) into a 26-byte wire packet.
    ms must have fields: dx, dy, dz, da (int), interval (int), flags (int).
    """
    body = struct.pack("<B iiii I B 3x",
        MAGIC_MICROSEG,
        ms.dx, ms.dy, ms.dz, ms.da,
        ms.interval,
        ms.flags,
    )
    return body + bytes([_crc8(body)])


def stamp_seq(packet: bytes, seq: int) -> bytes:
    """
    Stamp a rolling 8-bit sequence number into pad byte [22] of a MicroSegment
    packet and recompute the CRC. The Pico only executes a packet whose seq
    matches the one it expects next; a stale Go-Back-N retransmit (packet it
    already accepted) is ACKed but NOT executed. Without this, any go-back
    after the Pico accepted in-flight packets duplicates motion — a permanent
    position offset.
    """
    if len(packet) != 26 or packet[0] != MAGIC_MICROSEG:
        raise ValueError("stamp_seq: not a MicroSegment packet")
    body = bytearray(packet[:25])
    body[22] = seq & 0xFF
    return bytes(body) + bytes([_crc8(body)])


def unpack_microsegment(data: bytes):
    """
    Unpack a 26-byte wire packet into a dict. Raises ValueError on bad magic/CRC.
    """
    if len(data) != 26:
        raise ValueError(f"Expected 26 bytes, got {len(data)}")
    if data[0] != MAGIC_MICROSEG:
        raise ValueError(f"Bad magic: 0x{data[0]:02X} (expected 0x{MAGIC_MICROSEG:02X})")
    if _crc8(data[:25]) != data[25]:
        raise ValueError("CRC mismatch")
    dx, dy, dz, da, interval, flags = struct.unpack_from("<iiii I B", data, 1)
    return dict(dx=dx, dy=dy, dz=dz, da=da, interval=interval, flags=flags)


# ── ToolConfig packer/unpacker ────────────────────────────────────────────────

def pack_tool_config(config: ToolConfig, seq_num: int) -> bytes:
    body = struct.pack("<B H B ffff",
        MAGIC_TOOL,
        seq_num,
        config.tool_type,
        config.feed_max,
        config.lift_kappa,
        config.lift_height,
        config.z_feed,
    )
    return body + bytes([_crc8(body)])


def unpack_tool_config(data: bytes):
    """Returns (ToolConfig, seq_num). Raises ValueError on bad magic/CRC."""
    if len(data) != 21:
        raise ValueError(f"Expected 21 bytes, got {len(data)}")
    if data[0] != MAGIC_TOOL:
        raise ValueError(f"Bad magic: 0x{data[0]:02X}")
    if _crc8(data[:20]) != data[20]:
        raise ValueError("CRC mismatch")
    seq_num, tool_type, feed_max, lift_kappa, lift_height, z_feed = \
        struct.unpack_from("<H B ffff", data, 1)
    return ToolConfig(tool_type, feed_max, lift_kappa, lift_height, z_feed), seq_num


# ── SplineTile packer/unpacker ────────────────────────────────────────────────

def pack_spline_tile(curve, seq_num: int, flags: int) -> bytes:
    """
    Pack a CubicBezier (from stage 1) into a 37-byte wire packet.
    curve must have fields: p0, p1, p2, p3 — each a (x, y) pair in mm.
    """
    pts = (
        curve.p0[0], curve.p0[1],
        curve.p1[0], curve.p1[1],
        curve.p2[0], curve.p2[1],
        curve.p3[0], curve.p3[1],
    )
    body = struct.pack("<B H B 8f", MAGIC_SPLINE, seq_num, flags, *pts)
    return body + bytes([_crc8(body)])


def unpack_spline_tile(data: bytes):
    """Returns (curve_dict, seq_num, flags). Raises ValueError on bad magic/CRC."""
    if len(data) != 37:
        raise ValueError(f"Expected 37 bytes, got {len(data)}")
    if data[0] != MAGIC_SPLINE:
        raise ValueError(f"Bad magic: 0x{data[0]:02X}")
    if _crc8(data[:36]) != data[36]:
        raise ValueError("CRC mismatch")
    seq_num, flags = struct.unpack_from("<H B", data, 1)
    x0,y0, x1,y1, x2,y2, x3,y3 = struct.unpack_from("<8f", data, 4)
    return dict(p0=(x0,y0), p1=(x1,y1), p2=(x2,y2), p3=(x3,y3)), seq_num, flags


# ── stream serialisers ────────────────────────────────────────────────────────

def serialise_microsegments(microsegments):
    """
    Host production mode: yield one 26-byte packet per MicroSegment.
    Input: iterable of MicroSegment namedtuples from stage 6.
    """
    for ms in microsegments:
        yield pack_microsegment(ms)


def serialise_spline_path(subpaths, tool_config=None):
    """
    Local production mode: yield ToolConfig + SplineTile packets.
    Input: list of subpaths (each a list of CubicBezier) from stage 3.
    Emits one optional ToolConfig then SplineTiles with correct START/END flags.
    """
    seq = 0
    if tool_config is not None:
        yield pack_tool_config(tool_config, seq)
        seq = (seq + 1) & 0xFFFF

    for subpath in subpaths:
        if not subpath:
            continue
        for i, curve in enumerate(subpath):
            flags = 0
            if i == 0:
                flags |= TILE_PATH_START
            if i == len(subpath) - 1:
                flags |= TILE_PATH_END
            yield pack_spline_tile(curve, seq, flags)
            seq = (seq + 1) & 0xFFFF


# ── jog builder ─────────────────────────────────────────────────────────────────

_MS = namedtuple("MS", ["dx", "dy", "dz", "da", "interval", "flags"])


def make_jog(steps, feed_sps, accel_sps2, f_cpu, v_start_sps=50.0):
    """
    Trapezoidal jog as a list of MicroSegment packets.

    steps      : (sx, sy, sz, sa) signed target step counts
    feed_sps   : cruise step rate of the MAJOR axis (steps/s)
    accel_sps2 : acceleration of the major axis (steps/s^2)

    Steps are packed into chunks (~10 ms of motion each) so the packet count
    stays small regardless of step count — a 10800-step A move becomes ~25
    packets instead of 10800. The Pico's Bresenham loop handles multi-step
    deltas identically to single-step ones.

    Velocity follows v = sqrt(v0^2 + 2*a*d) at the start of each chunk,
    giving a smooth trapezoidal ramp.
    """
    sx, sy, sz, sa = steps
    abss = [abs(sx), abs(sy), abs(sz), abs(sa)]
    major = max(abss)
    if major == 0:
        return []

    signs = [(1 if s >= 0 else -1) for s in steps]
    v0 = max(1.0, min(v_start_sps, feed_sps))

    # Trapezoid geometry (in major-axis steps)
    d_acc = (feed_sps**2 - v0**2) / (2.0 * accel_sps2)
    if 2 * d_acc > major:  # triangular — never reach cruise
        peak = math.sqrt(v0**2 + accel_sps2 * major)
        d_acc = (peak**2 - v0**2) / (2.0 * accel_sps2)
    d_dec = d_acc

    err = [major // 2] * 4   # Bresenham accumulators for minor axes
    packets = []
    n = 0

    while n < major:
        # velocity at start of this chunk
        if n < d_acc:
            v = math.sqrt(v0**2 + 2.0 * accel_sps2 * n)
        elif n >= major - d_dec:
            v = math.sqrt(v0**2 + 2.0 * accel_sps2 * (major - n))
        else:
            v = feed_sps
        v = max(v, v0)

        # Adaptive chunk: ~10 ms at current velocity. Small during accel/decel
        # so the interval is accurate; large at cruise for streaming efficiency.
        chunk_size = min(max(1, int(v / 100)), major - n)
        interval = max(1, min(int(f_cpu / v), f_cpu))

        # per-axis deltas for this chunk via Bresenham
        delta = [0, 0, 0, 0]
        for ax in range(4):
            if abss[ax] == 0:
                continue
            if abss[ax] == major:
                delta[ax] = signs[ax] * chunk_size
            else:
                count = 0
                for _ in range(chunk_size):
                    err[ax] += abss[ax]
                    if err[ax] >= major:
                        err[ax] -= major
                        count += 1
                delta[ax] = signs[ax] * count

        n += chunk_size
        flags = MSEG_FLAG_PATH_END if n >= major else MSEG_FLAG_NONE
        packets.append(pack_microsegment(
            _MS(dx=delta[0], dy=delta[1], dz=delta[2], da=delta[3],
                interval=interval, flags=flags)))

    return packets
