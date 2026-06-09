"""
Stage 1: SVG path -> list of cubic Beziers.
Handles M, L, H, V, C, S, Q, Z (absolute and relative).
Output: list of CubicBezier(p0,p1,p2,p3) namedtuples, coordinates in SVG pixels.
"""

import re
from collections import namedtuple
from xml.etree import ElementTree as ET

CubicBezier = namedtuple("CubicBezier", ["p0", "p1", "p2", "p3"])

# ── path tokeniser ────────────────────────────────────────────────────────────

_CMD_RE = re.compile(r"([MmLlHhVvCcSsQqZz])|([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)")

def _tokenise(d):
    tokens = []
    for m in _CMD_RE.finditer(d):
        if m.group(1):
            tokens.append(m.group(1))
        else:
            tokens.append(float(m.group(2)))
    return tokens

def _consume(it, n):
    return tuple(next(it) for _ in range(n))

# ── primitive converters ──────────────────────────────────────────────────────

def _line_to_cubic(p0, p1):
    # degenerate cubic: control points on the line
    d = ((p1[0] - p0[0]) / 3, (p1[1] - p0[1]) / 3)
    return CubicBezier(p0, (p0[0] + d[0], p0[1] + d[1]),
                           (p1[0] - d[0], p1[1] - d[1]), p1)

def _quad_to_cubic(p0, qp1, p2):
    # degree elevation: C1 = P0 + 2/3*(QP1-P0), C2 = P2 + 2/3*(QP1-P2)
    c1 = (p0[0] + 2/3 * (qp1[0] - p0[0]), p0[1] + 2/3 * (qp1[1] - p0[1]))
    c2 = (p2[0] + 2/3 * (qp1[0] - p2[0]), p2[1] + 2/3 * (qp1[1] - p2[1]))
    return CubicBezier(p0, c1, c2, p2)

# ── main parser ───────────────────────────────────────────────────────────────

def path_to_subpaths(d):
    """
    Parse an SVG path d attribute and return list[list[CubicBezier]].
    Each M/m command that is not the first starts a new subpath.
    cur is preserved across subpath boundaries so relative m works correctly.
    """
    tokens = _tokenise(d)
    nums = []
    cmds = []
    for t in tokens:
        if isinstance(t, str):
            cmds.append((t, len(nums)))
        else:
            nums.append(t)
    cmds.append(("__end__", len(nums)))

    subpaths = []
    current = []
    cur = (0.0, 0.0)
    start = (0.0, 0.0)
    last_cp = None
    last_cmd = None
    first_cmd = True

    for ci, (cmd, ni) in enumerate(cmds[:-1]):
        next_ni = cmds[ci + 1][1]
        chunk = nums[ni:next_ni]
        rel = cmd.islower()
        C = cmd.upper()

        def abs_pt(x, y, _rel=rel, _cur=None):
            c = _cur if _cur is not None else cur
            if _rel:
                return (c[0] + x, c[1] + y)
            return (x, y)

        # New subpath boundary: M/m after the very first command
        if C == "M" and not first_cmd:
            if current:
                subpaths.append(current)
            current = []

        i = 0
        while i < len(chunk) or C == "Z":
            if C == "M":
                x, y = chunk[i], chunk[i+1]; i += 2
                cur = (cur[0] + x, cur[1] + y) if rel else (x, y)
                start = cur
                last_cp = None
                C = "L"
                if i >= len(chunk):
                    break
                continue

            elif C == "L":
                x, y = chunk[i], chunk[i+1]; i += 2
                p1 = (cur[0] + x, cur[1] + y) if rel else (x, y)
                current.append(_line_to_cubic(cur, p1))
                cur = p1; last_cp = None

            elif C == "H":
                x = chunk[i]; i += 1
                p1 = (cur[0] + x if rel else x, cur[1])
                current.append(_line_to_cubic(cur, p1))
                cur = p1; last_cp = None

            elif C == "V":
                y = chunk[i]; i += 1
                p1 = (cur[0], cur[1] + y if rel else y)
                current.append(_line_to_cubic(cur, p1))
                cur = p1; last_cp = None

            elif C == "C":
                x1,y1,x2,y2,x,y = chunk[i:i+6]; i += 6
                if rel:
                    p1 = (cur[0]+x1, cur[1]+y1)
                    p2 = (cur[0]+x2, cur[1]+y2)
                    p3 = (cur[0]+x,  cur[1]+y)
                else:
                    p1, p2, p3 = (x1,y1), (x2,y2), (x,y)
                current.append(CubicBezier(cur, p1, p2, p3))
                last_cp = p2; cur = p3

            elif C == "S":
                x2,y2,x,y = chunk[i:i+4]; i += 4
                if last_cmd in ("C","c","S","s") and last_cp is not None:
                    p1 = (2*cur[0] - last_cp[0], 2*cur[1] - last_cp[1])
                else:
                    p1 = cur
                p2 = (cur[0]+x2, cur[1]+y2) if rel else (x2, y2)
                p3 = (cur[0]+x,  cur[1]+y)  if rel else (x,  y)
                current.append(CubicBezier(cur, p1, p2, p3))
                last_cp = p2; cur = p3

            elif C == "Q":
                x1,y1,x,y = chunk[i:i+4]; i += 4
                qp1 = (cur[0]+x1, cur[1]+y1) if rel else (x1, y1)
                p2  = (cur[0]+x,  cur[1]+y)  if rel else (x,  y)
                current.append(_quad_to_cubic(cur, qp1, p2))
                last_cp = qp1; cur = p2

            elif C == "Z":
                if cur != start:
                    current.append(_line_to_cubic(cur, start))
                cur = start; last_cp = None
                break

            else:
                break

        last_cmd = cmd
        first_cmd = False

    if current:
        subpaths.append(current)
    return subpaths


def path_to_cubics(d):
    """Flat list of CubicBeziers from an SVG path d attribute."""
    return [c for sp in path_to_subpaths(d) for c in sp]

# ── SVG file loader ───────────────────────────────────────────────────────────

SVG_NS = "http://www.w3.org/2000/svg"

# Cubic Bézier approximation constant for a quarter-circle arc
_KAPPA = 0.5522847498

def _float(elem, attr, default=0.0):
    v = elem.get(attr)
    return float(v) if v is not None else default

def _style_prop(elem, prop):
    """Resolve a paint property from the element's `style=` declaration first,
    then its presentation attribute. Returns a lowercased string or None."""
    style = elem.get("style", "")
    for decl in style.split(";"):
        if ":" in decl:
            k, v = decl.split(":", 1)
            if k.strip() == prop:
                return v.strip().lower()
    val = elem.get(prop)
    return val.strip().lower() if val else None

_NO_PAINT = ("none", "transparent")

def _is_paintable(elem):
    """
    True if the element would draw anything — i.e. it has a visible fill or
    stroke. SVG defaults fill to black and stroke to none, so an element with
    no paint info at all is paintable (black fill). It is dropped only when fill
    is explicitly none/transparent AND there is no real stroke. This filters
    Inkscape helper/bounding boxes (fill:none;stroke:none) out of the toolpath.

    Note: only the element's own style is inspected, not inherited group paint —
    fine for the common case; an inherited fill:none is left as a known gap.
    """
    fill = _style_prop(elem, "fill")
    stroke = _style_prop(elem, "stroke")
    if fill is None or fill not in _NO_PAINT:
        return True                      # default-black or explicit fill
    return bool(stroke) and stroke not in _NO_PAINT  # fill none -> need a stroke

_DRAWABLE_TAGS = ("path", "circle", "ellipse", "rect", "line", "polygon", "polyline")

def _circle_to_cubics(cx, cy, rx, ry):
    """Approximate an ellipse (or circle when rx==ry) with 4 cubic Béziers."""
    kx, ky = rx * _KAPPA, ry * _KAPPA
    # Four quarter-arcs, starting at right (3 o'clock), going clockwise
    quarters = [
        CubicBezier((cx+rx, cy),      (cx+rx, cy+ky),  (cx+kx, cy+ry),  (cx, cy+ry)),
        CubicBezier((cx, cy+ry),      (cx-kx, cy+ry),  (cx-rx, cy+ky),  (cx-rx, cy)),
        CubicBezier((cx-rx, cy),      (cx-rx, cy-ky),  (cx-kx, cy-ry),  (cx, cy-ry)),
        CubicBezier((cx, cy-ry),      (cx+kx, cy-ry),  (cx+rx, cy-ky),  (cx+rx, cy)),
    ]
    return quarters

def _rect_to_cubics(x, y, w, h, rx=0.0, ry=0.0):
    """Convert a rect (optionally rounded) to cubic Béziers."""
    if rx == 0.0 and ry == 0.0:
        # Sharp corners — four line segments as degenerate cubics
        corners = [
            (x,   y),   (x+w, y),
            (x+w, y+h), (x,   y+h),
        ]
        return [_line_to_cubic(corners[i], corners[(i+1) % 4]) for i in range(4)]
    # Rounded rect: clamp radii
    rx = min(rx, w / 2); ry = min(ry, h / 2)
    kx, ky = rx * _KAPPA, ry * _KAPPA
    # 8-segment path: 4 straight sides + 4 rounded corners
    return path_to_cubics(
        f"M {x+rx},{y} "
        f"H {x+w-rx} C {x+w-rx+kx},{y} {x+w},{y+ky} {x+w},{y+ry} "
        f"V {y+h-ry} C {x+w},{y+h-ry+ky} {x+w-rx+kx},{y+h} {x+w-rx},{y+h} "
        f"H {x+rx} C {x+rx-kx},{y+h} {x},{y+h-ry+ky} {x},{y+h-ry} "
        f"V {y+ry} C {x},{y+ry-ky} {x+rx-kx},{y} {x+rx},{y} Z"
    )

def load_svg(path):
    """Returns a flat list of CubicBeziers from all elements in the SVG."""
    subpaths = load_svg_subpaths(path)
    return [c for sp in subpaths for c in sp]

def load_svg_subpaths(path):
    """
    Returns list[list[CubicBezier]], one inner list per subpath.
    Each SVG primitive element is one subpath; <path> elements are split on M.
    """
    tree = ET.parse(path)
    root = tree.getroot()
    all_subpaths = []

    for elem in root.iter():
        tag = elem.tag.replace(f"{{{SVG_NS}}}", "")

        # Skip non-paintable geometry (e.g. Inkscape fill:none;stroke:none boxes)
        if tag in _DRAWABLE_TAGS and not _is_paintable(elem):
            continue

        if tag == "path":
            d = elem.get("d", "")
            if d:
                all_subpaths.extend(path_to_subpaths(d))

        elif tag in ("circle", "ellipse"):
            cx = _float(elem, "cx"); cy = _float(elem, "cy")
            if tag == "circle":
                r = _float(elem, "r"); rx = ry = r
            else:
                rx = _float(elem, "rx"); ry = _float(elem, "ry")
            if rx > 0 and ry > 0:
                all_subpaths.append(_circle_to_cubics(cx, cy, rx, ry))

        elif tag == "rect":
            x = _float(elem, "x"); y = _float(elem, "y")
            w = _float(elem, "width"); h = _float(elem, "height")
            rx = _float(elem, "rx"); ry = _float(elem, "ry") or rx
            if w > 0 and h > 0:
                all_subpaths.append(_rect_to_cubics(x, y, w, h, rx, ry))

        elif tag == "line":
            x1 = _float(elem, "x1"); y1 = _float(elem, "y1")
            x2 = _float(elem, "x2"); y2 = _float(elem, "y2")
            all_subpaths.append([_line_to_cubic((x1, y1), (x2, y2))])

        elif tag in ("polygon", "polyline"):
            pts_str = elem.get("points", "").strip()
            if pts_str:
                coords = [float(v) for v in re.split(r"[\s,]+", pts_str) if v]
                pts = [(coords[i], coords[i+1]) for i in range(0, len(coords)-1, 2)]
                if len(pts) >= 2:
                    segs = [_line_to_cubic(pts[i], pts[i+1]) for i in range(len(pts)-1)]
                    if tag == "polygon":
                        segs.append(_line_to_cubic(pts[-1], pts[0]))
                    all_subpaths.append(segs)

    return all_subpaths

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import sys, argparse
    parser = argparse.ArgumentParser(description="Stage 1: SVG path -> cubic Beziers")
    parser.add_argument("svg", help="Path to SVG file")
    args = parser.parse_args()
    curves = load_svg(args.svg)
    print(f"Loaded {len(curves)} cubic Bezier(s) from {args.svg}\n")
    for i, c in enumerate(curves):
        print(f"  [{i}] p0={c.p0}  p1={c.p1}  p2={c.p2}  p3={c.p3}")
