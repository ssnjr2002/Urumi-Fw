"""Tests for stage 7: SplineTile + ToolConfig serialisation."""

import sys, os, struct, math
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "data"))

from stage7 import (
    pack_spline_tile, unpack_spline_tile,
    pack_tool_config, unpack_tool_config,
    serialise_path, serialise_paths, _crc8,
    TILE_PATH_START, TILE_PATH_END, TILE_MERGE_WITH_PREV,
    TOOL_CUT, TOOL_JOG, TOOL_CONFIG_DEFAULT,
)
from stage1 import CubicBezier
from stage2 import load_svg_mm
from stage3 import enforce_c1
from mock_stage3 import CASES as S3_CASES

DATA = os.path.join(os.path.dirname(__file__), "..", "data")

def svg(name):
    return os.path.join(DATA, name)

def approx(a, b, tol=1e-4):
    return abs(a - b) < tol

SAMPLE_CURVE = CubicBezier((10.0,20.0),(30.0,40.0),(50.0,60.0),(70.0,80.0))

# ── CRC-8 ─────────────────────────────────────────────────────────────────────

def test_crc8_known_value():
    # CRC of empty is 0x00
    assert _crc8(b"") == 0x00

def test_crc8_changes_with_data():
    assert _crc8(b"\x01") != _crc8(b"\x02")

def test_crc8_detects_corruption():
    data = b"\xAB\x01\x00\x02" + b"\x00" * 32
    good_crc = _crc8(data)
    corrupted = data[:-1] + bytes([data[-1] ^ 0xFF])
    assert _crc8(corrupted) != good_crc

# ── SplineTile pack/unpack ────────────────────────────────────────────────────

def test_spline_tile_length():
    pkt = pack_spline_tile(SAMPLE_CURVE, 0, 0)
    assert len(pkt) == 37

def test_spline_tile_magic():
    pkt = pack_spline_tile(SAMPLE_CURVE, 0, 0)
    assert pkt[0] == 0xAB

def test_spline_tile_crc_valid():
    pkt = pack_spline_tile(SAMPLE_CURVE, 0, 0)
    assert _crc8(pkt[:36]) == pkt[36]

def test_spline_tile_seq_num():
    pkt = pack_spline_tile(SAMPLE_CURVE, 42, 0)
    seq = struct.unpack_from("<H", pkt, 1)[0]
    assert seq == 42

def test_spline_tile_flags():
    pkt = pack_spline_tile(SAMPLE_CURVE, 0, TILE_PATH_START | TILE_PATH_END)
    flags = pkt[3]
    assert flags == (TILE_PATH_START | TILE_PATH_END)

def test_spline_tile_roundtrip_coords():
    pkt = pack_spline_tile(SAMPLE_CURVE, 7, TILE_PATH_START)
    curve, seq, flags = unpack_spline_tile(pkt)
    assert seq == 7
    assert flags == TILE_PATH_START
    for orig, recovered in zip(
        [SAMPLE_CURVE.p0, SAMPLE_CURVE.p1, SAMPLE_CURVE.p2, SAMPLE_CURVE.p3],
        [curve.p0,        curve.p1,        curve.p2,        curve.p3],
    ):
        assert approx(orig[0], recovered[0], tol=1e-4)
        assert approx(orig[1], recovered[1], tol=1e-4)

def test_spline_tile_bad_magic_raises():
    pkt = bytearray(pack_spline_tile(SAMPLE_CURVE, 0, 0))
    pkt[0] = 0x00
    try:
        unpack_spline_tile(bytes(pkt))
        assert False, "Should have raised"
    except ValueError:
        pass

def test_spline_tile_bad_crc_raises():
    pkt = bytearray(pack_spline_tile(SAMPLE_CURVE, 0, 0))
    pkt[36] ^= 0xFF
    try:
        unpack_spline_tile(bytes(pkt))
        assert False, "Should have raised"
    except ValueError:
        pass

def test_spline_tile_seq_wraps():
    pkt = pack_spline_tile(SAMPLE_CURVE, 0xFFFF, 0)
    _, seq, _ = unpack_spline_tile(pkt)
    assert seq == 0xFFFF

# ── ToolConfig pack/unpack ────────────────────────────────────────────────────

def test_tool_config_length():
    pkt = pack_tool_config(TOOL_CONFIG_DEFAULT, 0)
    assert len(pkt) == 21

def test_tool_config_magic():
    pkt = pack_tool_config(TOOL_CONFIG_DEFAULT, 0)
    assert pkt[0] == 0xAC

def test_tool_config_crc_valid():
    pkt = pack_tool_config(TOOL_CONFIG_DEFAULT, 0)
    assert _crc8(pkt[:20]) == pkt[20]

def test_tool_config_roundtrip():
    from stage7 import ToolConfig
    config = ToolConfig(TOOL_CUT, 120.0, 2.5, 1.5, 15.0)
    pkt = pack_tool_config(config, 3)
    recovered, seq = unpack_tool_config(pkt)
    assert seq == 3
    assert recovered.tool_type == TOOL_CUT
    assert approx(recovered.feed_max,    120.0, tol=1e-3)
    assert approx(recovered.lift_kappa,  2.5,   tol=1e-4)
    assert approx(recovered.lift_height, 1.5,   tol=1e-4)
    assert approx(recovered.z_feed,      15.0,  tol=1e-3)

def test_tool_config_bad_crc_raises():
    pkt = bytearray(pack_tool_config(TOOL_CONFIG_DEFAULT, 0))
    pkt[20] ^= 0xFF
    try:
        unpack_tool_config(bytes(pkt))
        assert False, "Should have raised"
    except ValueError:
        pass

# ── serialise_path ────────────────────────────────────────────────────────────

def test_serialise_with_tool_config_count():
    curves = S3_CASES["c1_perfect"]
    packets = list(serialise_path(curves, tool_config=TOOL_CONFIG_DEFAULT))
    assert len(packets) == len(curves) + 1  # +1 for ToolConfig

def test_serialise_without_tool_config_count():
    curves = S3_CASES["c1_perfect"]
    packets = list(serialise_path(curves))
    assert len(packets) == len(curves)

def test_serialise_seq_increments():
    curves = S3_CASES["multi_bad_joins"]
    packets = list(serialise_path(curves, tool_config=TOOL_CONFIG_DEFAULT))
    for i, pkt in enumerate(packets):
        seq = struct.unpack_from("<H", pkt, 1)[0]
        assert seq == i

def test_serialise_first_tile_path_start():
    curves = S3_CASES["c1_perfect"]
    packets = list(serialise_path(curves))
    _, _, flags = unpack_spline_tile(packets[0])
    assert flags & TILE_PATH_START

def test_serialise_last_tile_path_end():
    curves = S3_CASES["c1_perfect"]
    packets = list(serialise_path(curves))
    _, _, flags = unpack_spline_tile(packets[-1])
    assert flags & TILE_PATH_END

def test_serialise_all_crcs_valid():
    curves = S3_CASES["multi_bad_joins"]
    packets = list(serialise_path(curves, tool_config=TOOL_CONFIG_DEFAULT))
    for i, pkt in enumerate(packets):
        assert _crc8(pkt[:-1]) == pkt[-1], f"packet {i} CRC failed"

def test_serialise_single_curve_both_flags():
    curves = S3_CASES["single_curve"]
    packets = list(serialise_path(curves))
    _, _, flags = unpack_spline_tile(packets[0])
    assert flags & TILE_PATH_START
    assert flags & TILE_PATH_END

def test_serialise_total_bytes():
    curves = S3_CASES["c1_perfect"]
    packets = list(serialise_path(curves, tool_config=TOOL_CONFIG_DEFAULT))
    total = sum(len(p) for p in packets)
    assert total == 21 + len(curves) * 37

# ── real SVG roundtrip ────────────────────────────────────────────────────────

def test_snake_svg_roundtrip():
    curves, _ = load_svg_mm(svg("test_snake.svg"))
    repaired, _ = enforce_c1(curves)
    packets = list(serialise_path(repaired, tool_config=TOOL_CONFIG_DEFAULT))

    # unpack and verify all tiles reconstruct the curves
    tile_packets = [p for p in packets if p[0] == 0xAB]
    assert len(tile_packets) == len(repaired)

    for orig, pkt in zip(repaired, tile_packets):
        recovered, _, _ = unpack_spline_tile(pkt)
        for op, rp in zip([orig.p0,orig.p1,orig.p2,orig.p3],
                          [recovered.p0,recovered.p1,recovered.p2,recovered.p3]):
            assert approx(op[0], rp[0], tol=1e-3)
            assert approx(op[1], rp[1], tol=1e-3)

# ── serialise_paths (multi-subpath) ──────────────────────────────────────────

def test_serialise_paths_each_subpath_has_start_end():
    subpaths = [S3_CASES["c1_perfect"], S3_CASES["single_curve"]]
    packets = [p for p in serialise_paths(subpaths) if p[0] == 0xAB]
    # first and last of each subpath must carry the right flags
    sp1 = packets[:2]
    sp2 = packets[2:]
    _, _, f0 = unpack_spline_tile(sp1[0]);  assert f0 & TILE_PATH_START
    _, _, f1 = unpack_spline_tile(sp1[-1]); assert f1 & TILE_PATH_END
    _, _, f2 = unpack_spline_tile(sp2[0]);  assert f2 & TILE_PATH_START
    _, _, f3 = unpack_spline_tile(sp2[-1]); assert f3 & TILE_PATH_END

def test_serialise_paths_seq_global():
    subpaths = [S3_CASES["c1_perfect"], S3_CASES["c1_perfect"]]
    packets = list(serialise_paths(subpaths, tool_config=TOOL_CONFIG_DEFAULT))
    for i, pkt in enumerate(packets):
        seq = struct.unpack_from("<H", pkt, 1)[0]
        assert seq == i

def test_serialise_paths_all_crcs():
    subpaths = [S3_CASES["c1_perfect"], S3_CASES["single_curve"]]
    for pkt in serialise_paths(subpaths, tool_config=TOOL_CONFIG_DEFAULT):
        assert _crc8(pkt[:-1]) == pkt[-1]

def test_serialise_paths_inner_tiles_no_start_end():
    # middle tiles of a multi-curve subpath should have neither flag
    subpaths = [S3_CASES["multi_bad_joins"]]  # 3 curves
    tiles = [p for p in serialise_paths(subpaths) if p[0] == 0xAB]
    _, _, mid_flags = unpack_spline_tile(tiles[1])
    assert not (mid_flags & TILE_PATH_START)
    assert not (mid_flags & TILE_PATH_END)

# ── load_svg_subpaths ─────────────────────────────────────────────────────────

def test_fish_subpath_count():
    import os
    fish = os.path.join(os.path.dirname(__file__), "..", "..", "fish.svg")
    if not os.path.exists(fish):
        return  # skip if fish.svg not present
    from stage1 import load_svg_subpaths
    subpaths = load_svg_subpaths(fish)
    # fish.svg has multiple distinct shapes — expect more than 1 subpath
    assert len(subpaths) > 1

def test_circle_svg_one_subpath():
    from stage1 import load_svg_subpaths
    subpaths = load_svg_subpaths(svg("test_circle.svg"))
    assert len(subpaths) == 1
    assert len(subpaths[0]) == 4  # 4-arc approximation

if __name__ == "__main__":
    tests = [v for k, v in list(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for t in tests:
        try:
            t()
            print(f"  PASS  {t.__name__}")
            passed += 1
        except Exception as e:
            print(f"  FAIL  {t.__name__}: {e}")
            failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(failed)
