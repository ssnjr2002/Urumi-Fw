"""
Stage 2: SVG pixel coordinates -> millimetres + Y-axis flip.
Reads viewBox and width/height from the SVG root, builds a transform,
applies it to every control point from stage 1.
Output: same CubicBezier list, coordinates in mm, machine origin at bottom-left.
"""

import re
import argparse
from xml.etree import ElementTree as ET
from pipeline.stages.stage1 import load_svg, CubicBezier

# ── unit conversion to mm ─────────────────────────────────────────────────────

_UNIT_TO_MM = {
    "mm": 1.0,
    "cm": 10.0,
    "in": 25.4,
    "pt": 25.4 / 72,
    "pc": 25.4 / 6,
    "px": 25.4 / 96,
    "":   25.4 / 96,  # unitless treated as px
}

_DIM_RE = re.compile(r"^\s*([+-]?[\d.]+(?:[eE][+-]?\d+)?)\s*(mm|cm|in|pt|pc|px)?\s*$")

def _to_mm(value_str):
    m = _DIM_RE.match(value_str)
    if not m:
        raise ValueError(f"Cannot parse dimension: {value_str!r}")
    val = float(m.group(1))
    unit = (m.group(2) or "").lower()
    return val * _UNIT_TO_MM[unit]

# ── viewBox + size parser ─────────────────────────────────────────────────────

SVG_NS = "http://www.w3.org/2000/svg"

def parse_viewport(svg_path):
    """
    Returns (vb_minx, vb_miny, vb_w, vb_h, width_mm, height_mm).
    Falls back to viewBox px == mm when width/height are absent.
    """
    root = ET.parse(svg_path).getroot()

    vb = root.get("viewBox", "").strip()
    if vb:
        vb_minx, vb_miny, vb_w, vb_h = map(float, vb.split())
    else:
        vb_minx, vb_miny = 0.0, 0.0
        vb_w = float(root.get("width",  "100"))
        vb_h = float(root.get("height", "100"))

    w_attr = root.get("width")
    h_attr = root.get("height")

    if w_attr and h_attr:
        width_mm  = _to_mm(w_attr)
        height_mm = _to_mm(h_attr)
    else:
        # no physical size declared — treat viewBox units as mm 1:1
        width_mm  = vb_w
        height_mm = vb_h

    return vb_minx, vb_miny, vb_w, vb_h, width_mm, height_mm

# ── coordinate transform ──────────────────────────────────────────────────────

def make_transform(vb_minx, vb_miny, vb_w, vb_h, width_mm, height_mm):
    """
    Returns a function pt_svg -> pt_mm that applies:
      1. viewBox offset (subtract minX, minY)
      2. scale to mm
      3. Y-axis flip (SVG +Y down -> machine +Y up)
    """
    sx = width_mm  / vb_w
    sy = height_mm / vb_h

    def transform(pt):
        x_mm = (pt[0] - vb_minx) * sx
        y_mm = height_mm - (pt[1] - vb_miny) * sy
        return (x_mm, y_mm)

    return transform

def apply_transform(curves, transform):
    return [
        CubicBezier(
            transform(c.p0),
            transform(c.p1),
            transform(c.p2),
            transform(c.p3),
        )
        for c in curves
    ]

# ── public entry point ────────────────────────────────────────────────────────

def load_svg_mm(svg_path):
    """Full stage 1+2 pipeline: SVG file -> cubic Beziers in mm (flat list)."""
    curves_px = load_svg(svg_path)
    viewport  = parse_viewport(svg_path)
    transform = make_transform(*viewport)
    return apply_transform(curves_px, transform), viewport

def load_svg_mm_subpaths(svg_path):
    """Full stage 1+2 pipeline: SVG file -> list[list[CubicBezier]] in mm."""
    from pipeline.stages.stage1 import load_svg_subpaths
    subpaths_px = load_svg_subpaths(svg_path)
    viewport    = parse_viewport(svg_path)
    transform   = make_transform(*viewport)
    return [apply_transform(sp, transform) for sp in subpaths_px], viewport

def load_svg_mm_layers(svg_path):
    """
    Layer-aware stage 1+2: SVG file -> ordered {layer_name: list[subpath]} in mm.
    The multi-tool ingest — each layer can be planned with its own tool.
    """
    from pipeline.stages.stage1 import load_svg_layers
    layers_px = load_svg_layers(svg_path)
    viewport  = parse_viewport(svg_path)
    transform = make_transform(*viewport)
    layers_mm = {name: [apply_transform(sp, transform) for sp in subs]
                 for name, subs in layers_px.items()}
    return layers_mm, viewport

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Stage 2: SVG coords -> mm + Y-flip")
    parser.add_argument("svg", help="Path to SVG file")
    args = parser.parse_args()

    curves_mm, (vb_minx, vb_miny, vb_w, vb_h, w_mm, h_mm) = load_svg_mm(args.svg)

    print(f"viewBox : ({vb_minx}, {vb_miny}, {vb_w}, {vb_h})")
    print(f"canvas  : {w_mm:.3f} x {h_mm:.3f} mm")
    print(f"scale   : sx={w_mm/vb_w:.4f}  sy={h_mm/vb_h:.4f}")
    print(f"curves  : {len(curves_mm)}\n")
    for i, c in enumerate(curves_mm):
        print(f"  [{i}] p0={c.p0}  p1={c.p1}  p2={c.p2}  p3={c.p3}")
