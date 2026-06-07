"""
Stage 7: Serialise repaired cubics -> binary SplineTile + ToolConfig packets.
This is the host-side wire format sent to the Pico over USB.

ToolConfig packet (21 bytes):
  0       1B   magic = 0xAC
  1       2B   seq_num (uint16 LE)
  3       1B   tool_type  (0=JOG, 1=CUT, 2=CREASE)
  4       4B   feed_max   (float32 LE, mm/s)
  8       4B   lift_kappa (float32 LE, 1/mm)
  12      4B   lift_height(float32 LE, mm)
  16      4B   z_feed     (float32 LE, mm/s)
  20      1B   CRC8

SplineTile packet (37 bytes):
  0       1B   magic = 0xAB
  1       2B   seq_num (uint16 LE)
  3       1B   flags   (bit0=MERGE_WITH_PREV, bit1=PATH_START, bit2=PATH_END)
  4      32B   control_points[4][2] as float32 pairs, LE (P0x,P0y...P3x,P3y)
  36      1B   CRC8
"""

import struct
import argparse
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from stage1 import CubicBezier
from stage2 import load_svg_mm
from stage3 import enforce_c1
from collections import namedtuple

# ── flags ─────────────────────────────────────────────────────────────────────

TILE_MERGE_WITH_PREV = 0x01
TILE_PATH_START      = 0x02
TILE_PATH_END        = 0x04

# ── tool types ────────────────────────────────────────────────────────────────

TOOL_JOG    = 0
TOOL_CUT    = 1
TOOL_CREASE = 2

# ── CRC-8 (polynomial 0x8C, industrial variant) ───────────────────────────────

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

# ── ToolConfig ────────────────────────────────────────────────────────────────

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

def pack_tool_config(config: ToolConfig, seq_num: int) -> bytes:
    payload = struct.pack("<H B ffff",
        seq_num,
        config.tool_type,
        config.feed_max,
        config.lift_kappa,
        config.lift_height,
        config.z_feed,
    )
    body = bytes([0xAC]) + payload          # magic + 17 bytes = 18 bytes
    crc  = _crc8(body)
    return body + bytes([crc])              # 19... wait, let's count:
    # magic(1) + seq(2) + tool_type(1) + feed_max(4) + lift_kappa(4)
    # + lift_height(4) + z_feed(4) = 20 bytes body, +1 CRC = 21 total ✓

def unpack_tool_config(data: bytes):
    """Returns (ToolConfig, seq_num) or raises ValueError on bad CRC."""
    if len(data) != 21:
        raise ValueError(f"Expected 21 bytes, got {len(data)}")
    if data[0] != 0xAC:
        raise ValueError(f"Bad magic: 0x{data[0]:02X}")
    if _crc8(data[:20]) != data[20]:
        raise ValueError("CRC mismatch")
    seq_num, tool_type, feed_max, lift_kappa, lift_height, z_feed = \
        struct.unpack_from("<H B ffff", data, 1)
    return ToolConfig(tool_type, feed_max, lift_kappa, lift_height, z_feed), seq_num

# ── SplineTile ────────────────────────────────────────────────────────────────

def pack_spline_tile(curve: CubicBezier, seq_num: int, flags: int) -> bytes:
    pts = (
        curve.p0[0], curve.p0[1],
        curve.p1[0], curve.p1[1],
        curve.p2[0], curve.p2[1],
        curve.p3[0], curve.p3[1],
    )
    payload = struct.pack("<H B 8f", seq_num, flags, *pts)
    body = bytes([0xAB]) + payload          # magic(1)+seq(2)+flags(1)+pts(32) = 36
    crc  = _crc8(body)
    return body + bytes([crc])              # 37 bytes total ✓

def unpack_spline_tile(data: bytes):
    """Returns (CubicBezier, seq_num, flags) or raises ValueError on bad CRC."""
    if len(data) != 37:
        raise ValueError(f"Expected 37 bytes, got {len(data)}")
    if data[0] != 0xAB:
        raise ValueError(f"Bad magic: 0x{data[0]:02X}")
    if _crc8(data[:36]) != data[36]:
        raise ValueError("CRC mismatch")
    seq_num, flags = struct.unpack_from("<H B", data, 1)
    x0,y0, x1,y1, x2,y2, x3,y3 = struct.unpack_from("<8f", data, 4)
    curve = CubicBezier((x0,y0),(x1,y1),(x2,y2),(x3,y3))
    return curve, seq_num, flags

# ── stream serialiser ─────────────────────────────────────────────────────────

def serialise_paths(subpaths, tool_config=None):
    """
    Serialise a list of subpaths (each a list[CubicBezier]) into packets.
    Emits one optional ToolConfig then SplineTiles with correct START/END flags
    per subpath. seq_num increments globally across all subpaths.
    The receiver should lift the tool on PATH_END and lower it on PATH_START.
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

def serialise_path(curves, tool_config=None, first_flags=None):
    """
    Serialise a list of CubicBeziers into a sequence of packets.
    Yields bytes objects: one ToolConfig (if provided) then one SplineTile per curve.
    seq_num increments from 0, wrapping at 65536.
    first_flags: override flags for first tile (e.g. TILE_PATH_START).
                 Last tile always gets TILE_PATH_END.
    """
    seq = 0

    if tool_config is not None:
        yield pack_tool_config(tool_config, seq)
        seq = (seq + 1) & 0xFFFF

    for i, curve in enumerate(curves):
        flags = 0
        if i == 0:
            flags |= (first_flags if first_flags is not None else TILE_PATH_START)
        if i == len(curves) - 1:
            flags |= TILE_PATH_END
        yield pack_spline_tile(curve, seq, flags)
        seq = (seq + 1) & 0xFFFF

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Stage 7: serialise to SplineTile packets")
    parser.add_argument("svg",         help="Path to SVG file")
    parser.add_argument("--out",       help="Write binary stream to file")
    parser.add_argument("--angle-tol", type=float, default=5.0)
    parser.add_argument("--gap-tol",   type=float, default=0.01)
    args = parser.parse_args()

    curves_mm, _ = load_svg_mm(args.svg)
    repaired, _  = enforce_c1(curves_mm, args.angle_tol, args.gap_tol)

    packets = list(serialise_path(repaired, tool_config=TOOL_CONFIG_DEFAULT))
    total_bytes = sum(len(p) for p in packets)

    print(f"Curves  : {len(repaired)}")
    print(f"Packets : {len(packets)}  ({total_bytes} bytes total)\n")
    for i, pkt in enumerate(packets):
        kind = "ToolConfig " if pkt[0] == 0xAC else "SplineTile "
        print(f"  [{i:3d}]  {kind}  {len(pkt):2d}B  seq={int.from_bytes(pkt[1:3],'little'):5d}"
              f"  CRC=0x{pkt[-1]:02X}  {'OK' if _crc8(pkt[:-1])==pkt[-1] else 'FAIL'}")

    if args.out:
        with open(args.out, "wb") as f:
            for pkt in packets:
                f.write(pkt)
        print(f"\nWrote {total_bytes} bytes to {args.out}")
