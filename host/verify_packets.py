"""
verify_packets.py — decode and verify binary RS485 packet stream

Usage (pipe):
  python svg_to_packets.py input.svg | python verify_packets.py
  python svg_to_packets.py input.svg | python verify_packets.py --plot
  python svg_to_packets.py input.svg | python verify_packets.py --serial COM3
  python svg_to_packets.py input.svg | python verify_packets.py --serial /dev/ttyUSB0

Read from file:
  python verify_packets.py --in file.bin
  python verify_packets.py --in file.bin --plot

On Windows stdin is opened in text mode which corrupts binary data.
This script always reads from sys.stdin.buffer (binary mode).
"""

import sys, os, argparse, struct, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "pipeline", "stages"))
from stage7 import unpack_spline_tile, unpack_tool_config, _crc8, TILE_PATH_START, TILE_PATH_END

MAGIC_SPLINE = 0xAB
MAGIC_TOOL   = 0xAC
PKT_SIZES    = {MAGIC_SPLINE: 37, MAGIC_TOOL: 21}


# ── framing reader ─────────────────────────────────────────────────────────────

def read_packets(src):
    """Read length-prefixed packets written by svg_to_packets.write_stream."""
    while True:
        header = src.read(2)
        if not header:
            break
        if len(header) < 2:
            print("WARN: truncated length header", file=sys.stderr)
            break
        (length,) = struct.unpack("<H", header)
        data = src.read(length)
        if len(data) < length:
            print(f"WARN: expected {length}B payload, got {len(data)}B", file=sys.stderr)
            break
        yield data


# ── verification ───────────────────────────────────────────────────────────────

class Stats:
    def __init__(self):
        self.total = self.ok = self.crc_fail = self.bad_magic = self.bad_size = 0
        self.tool_configs = []
        self.spline_tiles = []   # (curve, seq_num, flags)
        self.seq_errors   = 0

    def report(self):
        print(f"\n{'-'*50}")
        print(f"Packets total   : {self.total}")
        print(f"  OK            : {self.ok}")
        print(f"  CRC failures  : {self.crc_fail}")
        print(f"  Bad magic     : {self.bad_magic}")
        print(f"  Bad size      : {self.bad_size}")
        print(f"  Seq errors    : {self.seq_errors}")
        print(f"ToolConfig pkts : {len(self.tool_configs)}")
        print(f"SplineTile pkts : {len(self.spline_tiles)}")
        if self.tool_configs:
            tc, seq = self.tool_configs[0]
            print(f"\nTool            : {['JOG','CUT','CREASE'][tc.tool_type]}")
            print(f"Feed max        : {tc.feed_max} mm/s")
            print(f"Lift kappa      : {tc.lift_kappa} 1/mm")
        ok = self.crc_fail == 0 and self.bad_magic == 0 and self.seq_errors == 0
        print(f"\nResult          : {'PASS' if ok else 'FAIL'}")
        return ok


def verify_stream(src, verbose=False):
    stats = Stats()
    expected_seq = None

    for raw in read_packets(src):
        stats.total += 1
        magic = raw[0] if raw else None

        if magic not in PKT_SIZES:
            stats.bad_magic += 1
            print(f"  [{stats.total:4d}] BAD MAGIC 0x{magic:02X}", file=sys.stderr)
            continue

        expected_len = PKT_SIZES[magic]
        if len(raw) != expected_len:
            stats.bad_size += 1
            print(f"  [{stats.total:4d}] BAD SIZE {len(raw)} (expected {expected_len})", file=sys.stderr)
            continue

        crc_ok = _crc8(raw[:-1]) == raw[-1]
        if not crc_ok:
            stats.crc_fail += 1
            print(f"  [{stats.total:4d}] CRC FAIL  magic=0x{magic:02X}", file=sys.stderr)
            continue

        seq = int.from_bytes(raw[1:3], "little")
        if expected_seq is not None and seq != expected_seq:
            stats.seq_errors += 1
            print(f"  [{stats.total:4d}] SEQ ERROR got={seq} expected={expected_seq}", file=sys.stderr)
        expected_seq = (seq + 1) & 0xFFFF

        if magic == MAGIC_TOOL:
            tc, s = unpack_tool_config(raw)
            stats.tool_configs.append((tc, s))
            if verbose:
                print(f"  [{stats.total:4d}] ToolConfig  seq={s:5d}  tool={tc.tool_type}  feed={tc.feed_max}")
        else:
            curve, s, flags = unpack_spline_tile(raw)
            stats.spline_tiles.append((curve, s, flags))
            if verbose:
                flag_str = "|".join(f for f, b in [
                    ("START", flags & 0x02), ("END", flags & 0x04), ("MERGE", flags & 0x01)
                ] if b)
                print(f"  [{stats.total:4d}] SplineTile  seq={s:5d}  flags={flag_str or '-'}  "
                      f"p0=({curve.p0[0]:.2f},{curve.p0[1]:.2f})")

        stats.ok += 1

    return stats


# ── optional plot ──────────────────────────────────────────────────────────────

def plot_path(spline_tiles):
    try:
        import matplotlib.pyplot as plt
        import matplotlib.cm as cm
        import matplotlib.widgets as mwidgets
        import numpy as np
    except ImportError:
        print("matplotlib not installed — skipping plot", file=sys.stderr)
        return

    subpath_count = sum(1 for _, _, flags in spline_tiles if flags & TILE_PATH_START)
    colors = cm.tab10.colors if subpath_count <= 10 else cm.tab20.colors
    color_idx = -1
    current_color = colors[0]

    fig, ax = plt.subplots(figsize=(8, 9))
    plt.subplots_adjust(bottom=0.12)
    ax.set_aspect("equal")
    ax.set_title(f"Decoded spline path ({subpath_count} subpath(s))")

    cut_lines = []
    jog_lines = []
    last_end = None   # last point of the previous subpath (for jog drawing)

    for curve, seq, flags in spline_tiles:
        if flags & TILE_PATH_START:
            color_idx = (color_idx + 1) % len(colors)
            current_color = colors[color_idx]
            # Draw jog from previous subpath end to this subpath start
            if last_end is not None:
                jx = [last_end[0], curve.p0[0]]
                jy = [last_end[1], curve.p0[1]]
                ln, = ax.plot(jx, jy, color="gray", lw=0.6, linestyle="--", alpha=0.5)
                jog_lines.append(ln)

        t = np.linspace(0, 1, 40)
        p0, p1, p2, p3 = (np.array(curve.p0), np.array(curve.p1),
                          np.array(curve.p2), np.array(curve.p3))
        pts = ((1-t)**3)[:,None]*p0 + 3*((1-t)**2*t)[:,None]*p1 + \
              3*((1-t)*t**2)[:,None]*p2 + (t**3)[:,None]*p3
        ln, = ax.plot(pts[:,0], pts[:,1], color=current_color, lw=0.8)
        cut_lines.append(ln)

        if flags & TILE_PATH_END:
            last_end = tuple(curve.p3)

    ax.invert_yaxis()   # SVG Y-down convention

    # Checkbox to toggle jog visibility (only shown if there are jog lines)
    if jog_lines:
        ax_check = plt.axes([0.72, 0.02, 0.22, 0.06])
        check = mwidgets.CheckButtons(ax_check, ["Show jog paths"], [True])

        def _toggle(label):
            visible = check.get_status()[0]
            for ln in jog_lines:
                ln.set_visible(visible)
            fig.canvas.draw_idle()

        check.on_clicked(_toggle)

    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore")
        plt.tight_layout(rect=[0, 0.08, 1, 1])
    plt.show()


# ── optional serial replay ─────────────────────────────────────────────────────

def replay_serial(spline_tiles, tool_configs, port, baud=921600):
    try:
        import serial
    except ImportError:
        print("pyserial not installed — cannot replay to hardware", file=sys.stderr)
        sys.exit(1)

    print(f"Opening {port} @ {baud}…", file=sys.stderr)
    with serial.Serial(port, baud, timeout=1) as ser:
        if tool_configs:
            tc, seq = tool_configs[0]
            from stage7 import pack_tool_config
            ser.write(pack_tool_config(tc, seq))

        for curve, seq, flags in spline_tiles:
            from stage7 import pack_spline_tile
            ser.write(pack_spline_tile(curve, seq, flags))
            time.sleep(0.001)

    print("Replay complete.", file=sys.stderr)


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Decode and verify binary RS485 packet stream"
    )
    parser.add_argument("--in",     dest="infile", help="Read from file instead of stdin")
    parser.add_argument("--plot",   action="store_true", help="Plot decoded spline path")
    parser.add_argument("--serial", metavar="PORT",
                        help="Replay raw packets to serial port (e.g. COM3 or /dev/ttyUSB0)")
    parser.add_argument("--baud",   type=int, default=921600)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    if args.infile:
        src = open(args.infile, "rb")
    else:
        src = sys.stdin.buffer if hasattr(sys.stdin, "buffer") else sys.stdin

    stats = verify_stream(src, verbose=args.verbose)
    ok = stats.report()

    if args.infile:
        src.close()

    if args.plot:
        plot_path(stats.spline_tiles)

    if args.serial and ok:
        replay_serial(stats.spline_tiles, stats.tool_configs, args.serial, args.baud)

    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
