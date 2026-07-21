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

ACK  (3 bytes): [0xAA] [expectedSeq] [0x00]
  Cumulative: expectedSeq is the Pico's next-wanted wire seq, i.e. every packet
  with a lower seq has been accepted. The sender advances its window to this
  point, so a lost/stale ACK self-heals via the next one.
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
MAGIC_JOG        = 0xAE
MAGIC_TOOL       = 0xAC
MAGIC_SPLINE     = 0xAD

MAGIC_ACK        = 0xAA
MAGIC_NACK       = 0xBB

MAGIC_STATUS_REQ = 0xA5
MAGIC_STATUS_RSP = 0xA7
STATUS_RSP_SIZE  = 30
# 0xA6 was the 9-byte v1 frame. Retired, never emitted, and reserved rather than
# reused: the demux consumes fixed-length frames blind, so a version mismatch
# must fail as an unknown magic instead of mis-parsing 30 bytes as 9.
MAGIC_STATUS_RSP_V1 = 0xA6

# Binary `seqreset` (§4.3): one byte out, ACK(0) back. Keeps stream start on the
# data plane instead of the one-outstanding text plane. The ACK is a readiness
# signal, not a window advance — a session must not feed it to its advance logic
# (which would correctly ignore the zero delta anyway).
MAGIC_SEQRESET   = 0xA8

# Soft abort (§4.5): one byte, no reply. The Pico ramps to rest, flushes the
# ring and lands IDLE with position INTACT — unlike `stop`, which forfeits it.
MAGIC_ABORT      = 0xA9
NACK_ABORTING    = 0x07   # barrier, not an error: wait for IDLE and reopen

NACK_CRC         = 0x01
NACK_FULL        = 0x02
NACK_BAD_MAGIC   = 0x03
NACK_PAUSED      = 0x04
NACK_BAD_STATE   = 0x06

# Config transfer (src/rp2350/shared.h). CFG_DATA is the only inbound frame with
# an opaque variable-length payload — see the demux note in reader.py.
MAGIC_CFG_SET    = 0xB0   # host→Pico: header, then (on RDY) payload
MAGIC_CFG_GET    = 0xB1   # host→Pico: request the active blob
MAGIC_CFG_RDY    = 0xB2   # Pico→host: header accepted — send payload
MAGIC_CFG_ACK    = 0xB3   # Pico→host: blob committed
MAGIC_CFG_NACK   = 0xB4   # Pico→host: rejected — next byte is the reason
MAGIC_CFG_DATA   = 0xB5   # Pico→host: CFG_GET response header (9 + length)

CFG_DATA_HDR_SIZE = 9     # magic + length(4) + crc32(4)

CFG_NACK_CRC       = 0x01
CFG_NACK_TOO_BIG   = 0x02
CFG_NACK_BAD_STATE = 0x03
CFG_NACK_FLASH     = 0x04
CFG_NACK_TIMEOUT   = 0x05

# ── MicroSegment flags ────────────────────────────────────────────────────────
# One byte, one namespace (see docs/wire_protocol.md). Low bits are wire/firmware
# semantics; high bits (0x08 LIFT, 0x10 JOG; defined as MICRO_* in microsegment.py)
# are host planning hints the firmware masks off.

MSEG_FLAG_NONE     = 0x00
# RETIRED on the wire (§4.7) — the firmware no longer honours bit 0 and has it
# commented out of MSEG_FLAG_WIRE_MASK. The name is kept, and kept equal to 0,
# so the several call sites that OR it in become no-ops instead of import
# errors; it is deliberately NOT 0x01 any more, so nothing can set the bit by
# accident. Delete the name once those call sites are cleaned up.
#
# (Unrelated to pipeline.stages' PATH_END / MICRO_PATH_END, which are live
# planner-internal velocity boundary markers and are not affected.)
MSEG_FLAG_PATH_END = 0x00   # was 0x01
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

# ── binary status request/response (mirrors `getstate` AND `getpos`) ─────────
# docs/wire_protocol.md; firmware src/rp2350/core0/status.cpp.
#
# STATUS_REQ: [0xA5]                                          (1 byte, no CRC)
# STATUS_RSP: 30 bytes —
#   [0]      magic 0xA7
#   [1..5]   state, enabled, homed, alarm, running
#   [6..7]   bufCount   u16 LE
#   [8..23]  pos[4]     i32 LE   (x, y, z, a — steps)
#   [24]     expectedSeq
#   [25..28] queuedUs   u32 LE
#   [29]     CRC8 over [0..28]
#
# bufCount counts segments queued in the Pico's ring (including the one
# executing); queuedUs is the sum of their durations, which is what pacing
# actually wants — segment count says nothing about time when segment durations
# vary by orders of magnitude.
#
# pos and expectedSeq and queuedUs are PARSED BUT NOT YET CONSUMED. Position
# still comes from the text `getpos` path and jog pacing still dead-reckons; see
# docs/comms_architecture.md §5 for what should replace them.

# 29 bytes; the trailing CRC is appended/checked separately. "<" means packed —
# no alignment padding, so the i32 array at [8] needs no special handling.
_STATUS_FMT = "<BBBBBBH4iBI"


def pack_status_rsp(state: int, axes_enabled: int, axes_homed: int,
                    alarm: int, running: int, buf_count: int = 0,
                    pos=(0, 0, 0, 0), expected_seq: int = 0,
                    queued_us: int = 0) -> bytes:
    """Pack a status snapshot into the 30-byte STATUS_RSP wire format."""
    body = struct.pack(_STATUS_FMT, MAGIC_STATUS_RSP, state, axes_enabled,
                       axes_homed, alarm, running, buf_count,
                       pos[0], pos[1], pos[2], pos[3],
                       expected_seq & 0xFF, queued_us & 0xFFFFFFFF)
    return body + bytes([_crc8(body)])


def unpack_status_rsp(data: bytes) -> dict:
    """Unpack a 30-byte STATUS_RSP. Raises ValueError on bad magic/size/CRC."""
    if len(data) != STATUS_RSP_SIZE:
        raise ValueError(f"Expected {STATUS_RSP_SIZE} bytes, got {len(data)}")
    if data[0] == MAGIC_STATUS_RSP_V1:
        raise ValueError(
            "Pico is sending the retired 9-byte STATUS_RSP (0xA6) — firmware "
            "predates docs/comms_architecture.md §4.2. Reflash it.")
    if data[0] != MAGIC_STATUS_RSP:
        raise ValueError(f"Bad magic: 0x{data[0]:02X}")
    if _crc8(data[:-1]) != data[-1]:
        raise ValueError("CRC mismatch")
    (_, state, axes_enabled, axes_homed, alarm, running, buf_count,
     px, py, pz, pa, expected_seq, queued_us) = struct.unpack(_STATUS_FMT, data[:-1])
    return dict(state=state, axes_enabled=axes_enabled, axes_homed=axes_homed,
                alarm=alarm, running=running, buf_count=buf_count,
                pos=[px, py, pz, pa], expected_seq=expected_seq,
                queued_us=queued_us)


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
    MAGIC_JOG:      26,
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

def pack_jog(ms) -> bytes:
    """
    Pack a MicroSegment namedtuple into a 26-byte JOG wire packet.
    """
    body = struct.pack("<B iiii I B 3x",
        MAGIC_JOG,
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
    if len(packet) != 26 or packet[0] not in (MAGIC_MICROSEG, MAGIC_JOG):
        raise ValueError("stamp_seq: not a MicroSegment or Jog packet")
    body = bytearray(packet[:25])
    body[22] = seq & 0xFF
    return bytes(body) + bytes([_crc8(body)])


def with_flag(packet: bytes, flag: int) -> bytes:
    """
    Copy of a 26-byte MSEG/jog packet with `flag` OR'd into the flags byte [21]
    and the CRC recomputed. The sender uses this to mark a tool-change boundary
    with MSEG_FLAG_PAUSE without disturbing the packet's host hint bits.
    """
    if len(packet) != 26:
        raise ValueError("with_flag: not a 26-byte packet")
    body = bytearray(packet[:25])
    body[21] |= flag & 0xFF
    return bytes(body) + bytes([_crc8(body)])


def unpack_microsegment(data: bytes):
    """
    Unpack a 26-byte wire packet into a dict. Raises ValueError on bad magic/CRC.
    """
    if len(data) != 26:
        raise ValueError(f"Expected 26 bytes, got {len(data)}")
    if data[0] not in (MAGIC_MICROSEG, MAGIC_JOG):
        raise ValueError(f"Bad magic: 0x{data[0]:02X} (expected MSEG or JOG)")
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
        packets.append(pack_jog(
            _MS(dx=delta[0], dy=delta[1], dz=delta[2], da=delta[3],
                interval=interval, flags=flags)))

    return packets
