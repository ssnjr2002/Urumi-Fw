"""
sim_duplicates.py â€” simulate the Go-Back-N duplicate-execution bug.

Reconstructs the XY pen path from a MicroSegment .bin stream twice:
  1. clean â€” every packet executed exactly once
  2. faulty â€” at random points (simulating NACK_FULL go-backs where the Pico
     had already accepted packets behind the rejected one), a few packets are
     re-executed a second time

Plots both side by side. Pen-down segments only (MICRO_JOG / MICRO_LIFT
excluded from drawing, but their motion still moves the pen position â€”
duplicated jogs shift everything).
"""

import sys, os, struct, random, argparse

from host.protocol.packets import unpack_microsegment
from config import default as config_default

MICRO_JOG  = 0x04
MICRO_LIFT = 0x08


def read_packets(path):
    with open(path, "rb") as f:
        while True:
            header = f.read(2)
            if len(header) < 2:
                break
            (length,) = struct.unpack("<H", header)
            data = f.read(length)
            if len(data) < length:
                break
            yield data


def inject_duplicates(segs, n_events, dup_run, rng):
    """
    Simulate go-back duplications: at n_events random points, re-execute the
    previous dup_run packets (the host rewound past packets the Pico had
    already accepted).
    """
    points = sorted(rng.sample(range(dup_run, len(segs)), n_events))
    out, prev = [], 0
    for p in points:
        out.extend(segs[prev:p])
        out.extend(segs[p - dup_run:p])   # duplicated run
        prev = p
    out.extend(segs[prev:])
    return out


def trace(segs, machine):
    """Walk segments, un-invert, return list of pen-down polylines (mm)."""
    x = y = z = 0.0
    sx = -1.0 if machine.x.invert else 1.0
    sy = -1.0 if machine.y.invert else 1.0
    sz = -1.0 if machine.z.invert else 1.0
    pen_down = False
    lines, cur = [], []
    for s in segs:
        if s['flags'] & MICRO_LIFT:
            z += sz * s['dz'] / machine.z.steps_per_unit
            down = z < -1e-6   # lower takes z to -lift_height; raise returns to 0
            if down and not pen_down:
                cur = [(x, y)]
            elif not down and pen_down and len(cur) > 1:
                lines.append(cur); cur = []
            pen_down = down
            continue
        x += sx * s['dx'] / machine.x.steps_per_unit
        y += sy * s['dy'] / machine.y.steps_per_unit
        if s['flags'] & MICRO_JOG:
            if pen_down:           # jog with pen down would draw â€” record it
                cur.append((x, y))
            continue
        if pen_down:
            cur.append((x, y))
    if pen_down and len(cur) > 1:
        lines.append(cur)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binfile")
    ap.add_argument("--events",  type=int, default=8,
                    help="number of go-back duplication events")
    ap.add_argument("--run",     type=int, default=4,
                    help="packets re-executed per event")
    ap.add_argument("--seed",    type=int, default=42)
    ap.add_argument("--out",     default="sim_duplicates.png")
    args = ap.parse_args()

    machine = config_default().machine
    segs = [unpack_microsegment(p) for p in read_packets(args.binfile)]
    print(f"{len(segs)} MicroSegments loaded")

    rng = random.Random(args.seed)
    faulty = inject_duplicates(segs, args.events, args.run, rng)
    print(f"faulty stream: {len(faulty)} segments "
          f"({args.events} events x {args.run} dup packets)")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(1, 2, figsize=(16, 6))
    for ax, (title, lines) in zip(axes, [
        ("clean (no duplicates)", trace(segs, machine)),
        (f"with {args.events} go-back duplications of {args.run} packets",
         trace(faulty, machine)),
    ]):
        for ln in lines:
            xs, ys = zip(*ln)
            ax.plot(xs, ys, lw=0.8, color="black")
        ax.set_aspect("equal")
        ax.set_title(title)
    fig.tight_layout()
    fig.savefig(args.out, dpi=120)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()

