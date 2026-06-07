"""Tests for stage 1: SVG path -> cubic Beziers."""

import sys, os
sys.path.insert(0, os.path.dirname(__file__))
DATA = os.path.join(os.path.dirname(__file__), "..", "data")

from stage1 import load_svg, path_to_cubics

def svg(name):
    return os.path.join(DATA, name)

def approx(a, b, tol=1e-6):
    return all(abs(x - y) < tol for x, y in zip(a, b))

def test_snake():
    curves = load_svg(svg("test_snake.svg"))
    assert len(curves) == 2
    assert approx(curves[0].p0, (10, 50))
    assert approx(curves[0].p3, (70, 50))
    assert approx(curves[1].p0, (70, 50))   # chains
    assert approx(curves[1].p3, (130, 50))

def test_mixed_commands():
    # test.svg: L -> degenerate cubic, Q -> elevated cubic, Z -> close
    curves = load_svg(svg("test.svg"))
    assert len(curves) == 3
    # L: control points collinear with endpoints
    assert approx(curves[0].p0, (10, 10))
    assert approx(curves[0].p3, (90, 10))
    # Z: closes back to start
    assert approx(curves[2].p3, (10, 10))

def test_saturate_implicit_C_repeat():
    # Two cubics from one C command with implicit repetition
    curves = load_svg(svg("test_saturate.svg"))
    assert approx(curves[0].p3, (50, 10))
    assert approx(curves[1].p0, (50, 10))   # chains
    assert approx(curves[1].p3, (90, 10))

def test_saturate_S_reflection():
    # S after C: reflected control point
    curves = load_svg(svg("test_saturate.svg"))
    # curve[2] is C, curve[3] is S — p1 of [3] should be reflection of p2 of [2]
    c, s = curves[2], curves[3]
    reflected = (2*c.p3[0] - c.p2[0], 2*c.p3[1] - c.p2[1])
    assert approx(s.p1, reflected)

def test_saturate_S_no_preceding_C():
    # S with no preceding C: p1 falls back to current point (p0)
    curves = load_svg(svg("test_saturate.svg"))
    assert approx(curves[4].p0, curves[4].p1)

def test_saturate_Q_degree_elevation():
    # Q elevated to cubic: control points follow 2/3 rule
    curves = load_svg(svg("test_saturate.svg"))
    q = curves[6]
    # for Q p0=(10,80) qp1=(50,60) p3=(90,80):
    # c1 = p0 + 2/3*(qp1-p0), c2 = p3 + 2/3*(qp1-p3)
    p0, p3 = (10, 80), (90, 80)
    qp1 = (50, 60)
    c1 = (p0[0] + 2/3*(qp1[0]-p0[0]), p0[1] + 2/3*(qp1[1]-p0[1]))
    c2 = (p3[0] + 2/3*(qp1[0]-p3[0]), p3[1] + 2/3*(qp1[1]-p3[1]))
    assert approx(q.p1, c1)
    assert approx(q.p2, c2)

def test_relative_subpath():
    # Relative m opens new subpath at correct absolute position
    curves = load_svg(svg("test_saturate.svg"))
    # curve[27]: second subpath from "m 0 20" after L to (150,10)
    assert approx(curves[27].p0, (150, 30))

def test_total_saturate_count():
    curves = load_svg(svg("test_saturate.svg"))
    assert len(curves) == 29

def test_circle_element():
    # <circle cx=50 cy=50 r=40> -> 4 cubic Béziers approximating full circle
    import io, sys
    from xml.etree import ElementTree as ET
    from stage1 import _circle_to_cubics, _KAPPA
    curves = _circle_to_cubics(50, 50, 40, 40)
    assert len(curves) == 4
    # Each segment starts where the previous one ends
    for i in range(4):
        assert approx(curves[i].p3, curves[(i+1) % 4].p0)
    # Start point should be (cx+r, cy) = (90, 50)
    assert approx(curves[0].p0, (90, 50))
    # Control arm length should be r * kappa
    kx = 40 * _KAPPA
    assert approx(curves[0].p1, (90, 50 + kx))

def test_circle_via_load_svg():
    curves = load_svg(svg("test_circle.svg"))
    assert len(curves) == 4
    assert approx(curves[-1].p3, curves[0].p0)

def test_ellipse_element():
    curves = load_svg(svg("test_ellipse.svg"))
    assert len(curves) == 4
    # cx=50 cy=30 rx=40 ry=20 — rightmost point is (cx+rx, cy)
    assert approx(curves[0].p0, (90, 30))
    assert approx(curves[-1].p3, curves[0].p0)

def test_rect_sharp():
    curves = load_svg(svg("test_rect.svg"))
    assert len(curves) == 4
    # x=10 y=20 w=80 h=60 -> corners (10,20)->(90,20)->(90,80)->(10,80)->back
    assert approx(curves[0].p0, (10, 20))
    assert approx(curves[0].p3, (90, 20))
    assert approx(curves[3].p3, (10, 20))

def test_line_element():
    curves = load_svg(svg("test_line.svg"))
    assert len(curves) == 1
    assert approx(curves[0].p0, (0, 0))
    assert approx(curves[0].p3, (100, 50))
    assert approx(curves[0].p1, (100/3, 50/3))

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
