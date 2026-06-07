# host — SVG to RS485 packet pipeline

CLI tools for converting an SVG file into a verified binary RS485 packet stream, with optional hardware replay.

## Requirements

- Python 3.13+
- [uv](https://docs.astral.sh/uv/) for dependency management

Install dependencies (first time only):

```
uv sync
```

## Usage

### PowerShell (Windows) — use run.ps1

PowerShell 5 corrupts binary data in pipes between native processes. Use the provided wrapper script instead:

```powershell
.\run.ps1 ..\fish.svg
.\run.ps1 ..\fish.svg -Plot
.\run.ps1 ..\fish.svg -Plot -V
.\run.ps1 ..\fish.svg -Serial COM3
```

`run.ps1` routes data through a temp file so no binary crosses a PowerShell pipe.

### cmd.exe / bash / Linux — pipe directly

```
uv run --quiet python svg_to_packets.py <input.svg> | uv run --quiet python verify_packets.py [options]
```

> **Important:** always pass `--quiet` when piping. Without it, uv may write startup output to stdout which corrupts the binary framing.

### File mode (works everywhere)

```
uv run python svg_to_packets.py input.svg --out output.bin
uv run python verify_packets.py --in output.bin
```

---

## svg_to_packets.py

Runs the full pipeline (SVG → splines → C1 repair → binary RS485 frames) and writes a length-prefixed binary stream to stdout or a file.

```
uv run python svg_to_packets.py <svg> [--out FILE] [--angle-tol DEG] [--gap-tol MM]
```

| Option | Default | Description |
|---|---|---|
| `svg` | — | Input SVG file |
| `--out FILE` | stdout | Write binary stream to file instead of stdout |
| `--angle-tol DEG` | 5.0 | C1 angle tolerance for corner blending (degrees) |
| `--gap-tol MM` | 0.01 | Gap tolerance for bridging discontinuities (mm) |

---

## verify_packets.py

Reads a binary packet stream, validates every CRC and sequence number, and reports PASS/FAIL.

```
uv run python verify_packets.py [--in FILE] [--plot] [--serial PORT] [--baud RATE] [-v]
```

| Option | Default | Description |
|---|---|---|
| `--in FILE` | stdin | Read from file instead of stdin |
| `--plot` | off | Plot decoded spline path (requires matplotlib) |
| `--serial PORT` | off | Replay verified packets to hardware (e.g. `COM3`, `/dev/ttyUSB0`) |
| `--baud RATE` | 921600 | Serial baud rate |
| `-v` | off | Verbose packet-by-packet output |

The plot window shows each subpath in a distinct color. Dashed gray lines show tool jog moves between subpaths. A checkbox in the plot toggles jog visibility.

---

## Examples

```powershell
# PowerShell — use run.ps1
.\run.ps1 ..\fish.svg -Plot
.\run.ps1 ..\fish.svg -Serial COM3
.\run.ps1 ..\fish.svg -Plot -V
```

```bash
# cmd.exe / bash / Linux
uv run --quiet python svg_to_packets.py ../fish.svg | uv run --quiet python verify_packets.py --plot

# File mode (works everywhere, no --quiet needed)
uv run python svg_to_packets.py ../fish.svg --out fish.bin
uv run python verify_packets.py --in fish.bin -v
```

---

## Supported SVG elements

| Element | Handling |
|---|---|
| `<path>` | Full support — M L H V C S Q Z (absolute and relative) |
| `<circle>` | 4-arc cubic Bézier approximation |
| `<ellipse>` | 4-arc cubic Bézier approximation |
| `<rect>` | 4 line segments (rounded corners supported via rx/ry) |
| `<line>` | Single degenerate cubic |

Each element and each `M` command within a `<path>` is treated as a separate subpath. `enforce_c1` runs per subpath — shapes are never bridged together.

---

## Packet format

Frames on the wire use the protocol defined in `include/common.h` and `pipeline/stages/stage7.py`:

| Packet | Size | Magic |
|---|---|---|
| `ToolConfig` | 21 B | `0xAC` |
| `SplineTile` | 37 B | `0xAB` |

The pipe framing adds a 2-byte little-endian length prefix before each packet (stripped by `verify_packets.py` before CRC checking).
