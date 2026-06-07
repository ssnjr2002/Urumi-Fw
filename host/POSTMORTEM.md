# Host Pipeline Post-Mortem

**Date:** 2026-06-07
**Branch:** motion-plan

---

## What was built

Two CLI scripts forming a verifiable binary pipeline:

```
svg_to_packets.py input.svg | verify_packets.py [--plot] [--serial PORT]
```

`svg_to_packets.py` runs the full stage 1→7 pipeline and streams length-prefixed binary RS485 frames to stdout. `verify_packets.py` decodes the stream, validates every CRC and sequence number, and optionally plots the decoded path or replays raw packets to hardware over serial.

---

## What went wrong

**SVG primitives missing from stage1.** `test_circle.svg` produced 0 SplineTiles on the first run because stage1 only handled `<path>` elements. `<circle>`, `<ellipse>`, `<rect>`, and `<line>` had to be added before the pipeline produced any output.

**Subpath flattening produced jog artifacts.** The initial implementation treated all curves from an SVG as one continuous path. `enforce_c1` bridged gaps between distinct shapes with line segments, producing long diagonal traversals across the canvas. The fix was to split on M commands and run `enforce_c1` per subpath, then emit proper `PATH_START`/`PATH_END` flags via `serialise_paths`.

**Relative `m` broke when splitting on M boundaries.** The first attempt at subpath splitting used a regex to split the `d` string at each `M/m` and called `path_to_cubics` on each segment independently. This lost the `cur` position context, so relative `m` resolved from `(0,0)` instead of the previous endpoint. Fixed by moving the full parser into `path_to_subpaths` so `cur` is preserved across subpath boundaries.

**Windows binary pipe corruption.** `sys.stdout` and `sys.stdin` default to text mode on Windows, which applies CR/LF translation and encoding filters that silently corrupt binary data. Fixed by using `sys.stdout.buffer` / `sys.stdin.buffer` throughout.

**Windows cp1252 encoding error.** The `─` box-drawing character in the verify report caused a `UnicodeEncodeError` on Windows stdout. Replaced with plain `-`.

---

## What worked well

- Piping binary between scripts mirrors the real production flow exactly — the CRC check in `verify_packets` runs on the actual wire bytes, not a simulation layer.
- Length-prefixed framing (2-byte LE before each packet) made the pipe robust without needing sentinel values or fixed-size reads.
- Per-subpath coloring in the plot immediately exposed the jog artifact visually before any analysis was needed.
- `uv init` in the `host/` folder kept dependencies isolated from the rest of the repo.

---

## Current state

Stages 1, 2, 3, and 7 are complete and tested. The host pipeline produces verified binary output from any SVG. Hardware replay (`--serial`) is implemented but untested against real hardware.

**Known gap:** the bounding `<rect>` present in some SVGs (e.g. `fish.svg`) is included as a subpath and generates a long jog to a canvas corner. Filtering or classifying non-cut elements is not yet implemented.
