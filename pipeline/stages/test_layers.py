"""
Tests for layer-aware SVG ingest (multi-tool step 8a).

Confirms subpaths group by their <g> layer, the flat loader stays the union of
all layers (single-tool path unchanged), the non-paintable bbox is filtered, and
layer names resolve to tool profiles by convention.
"""

import sys, os
sys.path.insert(0, os.path.dirname(__file__))

from stage1 import load_svg_layers, load_svg_subpaths
from stage2 import load_svg_mm_layers
from config import tool_for_layer, KNIFE, CREASE, PEN

SVG = os.path.join(os.path.dirname(__file__), "..", "data", "test_layers.svg")


def test_layers_grouped_in_order():
    layers = load_svg_layers(SVG)
    assert list(layers.keys()) == ["knife", "crease"]   # document order
    assert len(layers["knife"]) == 2                     # path + rect
    assert len(layers["crease"]) == 1                    # one path


def test_nonpaintable_bbox_filtered():
    layers = load_svg_layers(SVG)
    assert "" not in layers          # the fill:none;stroke:none bbox produced nothing


def test_flat_loader_is_union_of_layers():
    layers = load_svg_layers(SVG)
    flat = load_svg_subpaths(SVG)
    assert len(flat) == sum(len(v) for v in layers.values())   # 3


def test_mm_layers_same_keys():
    layers_mm, _ = load_svg_mm_layers(SVG)
    assert list(layers_mm.keys()) == ["knife", "crease"]
    # transform applied: knife's first path runs y=10 in svg -> y=90mm (flip)
    first_curve = layers_mm["knife"][0][0]
    assert abs(first_curve.p0[1] - 90.0) < 1e-6


def test_layer_to_tool_resolution():
    assert tool_for_layer("knife") is KNIFE
    assert tool_for_layer("Crease") is CREASE        # case-insensitive
    assert tool_for_layer("pen") is PEN
    assert tool_for_layer("nonsense") is None         # unresolved
    assert tool_for_layer("cut", overrides={"cut": KNIFE}) is KNIFE


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    passed = failed = 0
    for t in tests:
        try:
            t(); print(f"  PASS  {t.__name__}"); passed += 1
        except Exception as e:
            print(f"  FAIL  {t.__name__}: {e}"); failed += 1
    print(f"\n{passed} passed, {failed} failed")
    sys.exit(failed)
