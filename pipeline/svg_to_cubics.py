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

def path_to_cubics(d):
    tokens = _tokenise(d)
    it = iter(tokens)
    curves = []

    cur = (0.0, 0.0)   # current point
    start = (0.0, 0.0) # subpath start (for Z)
    last_cp = None      # last control point (for S/T reflection)
    last_cmd = None

    cmd = None
    pending = list(tokens)
    idx = 0

    # re-parse with index for implicit command repetition
    nums = []
    cmds = []
    for t in tokens:
        if isinstance(t, str):
            cmds.append((t, len(nums)))
        else:
            nums.append(t)
    cmds.append(("__end__", len(nums)))

    def n(i):
        return nums[i]

    for ci, (cmd, ni) in enumerate(cmds[:-1]):
        next_ni = cmds[ci + 1][1]
        chunk = nums[ni:next_ni]
        rel = cmd.islower()
        C = cmd.upper()

        def abs_pt(x, y):
            if rel:
                return (cur[0] + x, cur[1] + y)
            return (x, y)

        i = 0
        while i < len(chunk) or (C == "Z"):
            if C == "M":
                x, y = chunk[i], chunk[i+1]; i += 2
                cur = abs_pt(x, y)
                start = cur
                last_cp = None
                # subsequent coords in M are implicit L
                C = "L"; rel = cmd.islower()
                if i >= len(chunk):
                    break
                continue

            elif C == "L":
                x, y = chunk[i], chunk[i+1]; i += 2
                p1 = abs_pt(x, y)
                curves.append(_line_to_cubic(cur, p1))
                cur = p1; last_cp = None

            elif C == "H":
                x = chunk[i]; i += 1
                p1 = (cur[0] + x if rel else x, cur[1])
                curves.append(_line_to_cubic(cur, p1))
                cur = p1; last_cp = None

            elif C == "V":
                y = chunk[i]; i += 1
                p1 = (cur[0], cur[1] + y if rel else y)
                curves.append(_line_to_cubic(cur, p1))
                cur = p1; last_cp = None

            elif C == "C":
                x1,y1,x2,y2,x,y = chunk[i:i+6]; i += 6
                p1 = abs_pt(x1, y1)
                p2 = abs_pt(x2, y2)
                p3 = abs_pt(x, y)
                curves.append(CubicBezier(cur, p1, p2, p3))
                last_cp = p2; cur = p3

            elif C == "S":
                x2,y2,x,y = chunk[i:i+4]; i += 4
                # reflect last control point
                if last_cmd in ("C","c","S","s") and last_cp is not None:
                    p1 = (2*cur[0] - last_cp[0], 2*cur[1] - last_cp[1])
                else:
                    p1 = cur
                p2 = abs_pt(x2, y2)
                p3 = abs_pt(x, y)
                curves.append(CubicBezier(cur, p1, p2, p3))
                last_cp = p2; cur = p3

            elif C == "Q":
                x1,y1,x,y = chunk[i:i+4]; i += 4
                qp1 = abs_pt(x1, y1)
                p2  = abs_pt(x, y)
                curves.append(_quad_to_cubic(cur, qp1, p2))
                last_cp = qp1; cur = p2

            elif C == "Z":
                if cur != start:
                    curves.append(_line_to_cubic(cur, start))
                cur = start; last_cp = None
                break

            else:
                break  # unknown command, skip

        last_cmd = cmd

    return curves

# ── SVG file loader ───────────────────────────────────────────────────────────

SVG_NS = "http://www.w3.org/2000/svg"

def load_svg(path):
    tree = ET.parse(path)
    root = tree.getroot()
    all_curves = []
    for elem in root.iter(f"{{{SVG_NS}}}path"):
        d = elem.get("d", "")
        if d:
            all_curves.extend(path_to_cubics(d))
    return all_curves

# ── main ──────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    import sys, os
    svg = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "test_snake.svg")
    curves = load_svg(svg)
    print(f"Loaded {len(curves)} cubic Bezier(s) from {os.path.basename(svg)}\n")
    for i, c in enumerate(curves):
        print(f"  [{i}] p0={c.p0}  p1={c.p1}  p2={c.p2}  p3={c.p3}")
