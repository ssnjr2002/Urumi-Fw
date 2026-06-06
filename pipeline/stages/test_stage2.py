"""Tests for stage 2: SVG pixel coords -> mm + Y-axis flip."""

import sys, os
sys.path.insert(0, os.path.dirname(__file__))
DATA = os.path.join(os.path.dirname(__file__), "..", "data")

from stage2 import load_svg_mm, parse_viewport

def svg(name):
    return os.path.join(DATA, name)

def approx(a, b, tol=1e-3):
    return all(abs(x - y) < tol for x, y in zip(a, b))

def test_mm_units_scale():
    # viewBox 100x100, width/height 200mm -> scale 2.0
    _, vp = load_svg_mm(svg("coord_mm_units.svg"))
    vb_minx, vb_miny, vb_w, vb_h, w_mm, h_mm = vp
    assert w_mm == 200.0 and h_mm == 200.0
    assert w_mm / vb_w == 2.0

def test_mm_units_coords():
    # SVG (10,10) -> (20, 180)mm after 2x scale + Y-flip
    curves, _ = load_svg_mm(svg("coord_mm_units.svg"))
    assert approx(curves[0].p0, (20, 180))

def test_cm_units():
    # 10cm = 100mm, scale 1.0; SVG (10,10) -> (10, 90)mm
    curves, vp = load_svg_mm(svg("coord_cm_units.svg"))
    assert abs(vp[4] - 100.0) < 0.01   # width_mm
    assert approx(curves[0].p0, (10, 90))
    assert approx(curves[0].p3, (90, 10))

def test_px_units():
    # 377px at 96dpi ≈ 99.748mm; (0,0) -> (0, ~99.75)mm
    curves, vp = load_svg_mm(svg("coord_px_units.svg"))
    assert abs(vp[4] - 99.748) < 0.01
    assert approx(curves[0].p0, (0, 99.748), tol=0.01)
    assert approx(curves[0].p3, (99.748, 0), tol=0.01)

def test_nonzero_viewbox_origin():
    # viewBox="50 30 100 80": SVG (50,30) is canvas origin -> (0, 80)mm
    curves, _ = load_svg_mm(svg("coord_nonzero_origin.svg"))
    assert approx(curves[0].p0, (0, 80))
    assert approx(curves[0].p3, (100, 0))

def test_nonsquare():
    # viewBox 200x100 -> 100x50mm, scale 0.5; corners map correctly
    curves, vp = load_svg_mm(svg("coord_nonsquare.svg"))
    assert abs(vp[4] - 100.0) < 0.01 and abs(vp[5] - 50.0) < 0.01
    assert approx(curves[0].p0, (0, 50))    # SVG (0,0)   -> (0, 50)mm
    assert approx(curves[2].p0, (100, 0))   # SVG (200,100) -> (100, 0)mm

def test_no_size_fallback():
    # No width/height: viewBox px == mm 1:1, canvas 100x60mm
    curves, vp = load_svg_mm(svg("coord_no_size.svg"))
    assert vp[4] == 100.0 and vp[5] == 60.0
    assert approx(curves[0].p0, (0, 60))    # SVG (0,0) -> (0, 60)mm

def test_y_flip_top_edge():
    # SVG top edge (y=0) -> machine y = height_mm = 100
    curves, _ = load_svg_mm(svg("coord_yfliip_verify.svg"))
    top_edge = curves[0]
    assert approx(top_edge.p0, (0, 100))
    assert approx(top_edge.p3, (100, 100))

def test_y_flip_bottom_edge():
    # SVG bottom edge (y=100) -> machine y = 0
    curves, _ = load_svg_mm(svg("coord_yfliip_verify.svg"))
    bottom_edge = curves[1]
    assert approx(bottom_edge.p0, (0, 0))
    assert approx(bottom_edge.p3, (100, 0))

def test_y_flip_diagonal():
    # SVG (0,0) -> (0,100)mm; SVG (100,100) -> (100,0)mm
    curves, _ = load_svg_mm(svg("coord_yfliip_verify.svg"))
    diag = curves[3]
    assert approx(diag.p0, (0, 100))
    assert approx(diag.p3, (100, 0))

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
