"""
_inspect.py — scratch inspector for the host pipeline (knife/pen plans).

Usage:
  uv run python _inspect.py [svg] [--pen] [--feed F] [--amax A]

Reports: segment count, slowest segments, in-curve cusps (count + angles), and
any pen-down segment carrying a large rotation (which would tear material).
"""
import sys, os, argparse
base = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(base, "pipeline", "stages"))

from stage2 import load_svg_mm_subpaths
from stage3 import enforce_c1
from stage4 import compute_metrics
from stage5 import plan_velocities, PATH_START, PATH_END
from stage6 import (build_toolpath, evaluate_curve, _tangent_angle,
                    MICRO_JOG, MICRO_LIFT, MICRO_PATH_END)
from config import default, KNIFE, PEN

ap = argparse.ArgumentParser()
ap.add_argument("svg", nargs="?", default="fish_norect.svg")
ap.add_argument("--pen", action="store_true")
ap.add_argument("--feed", type=float, default=15.0)
ap.add_argument("--amax", type=float, default=200.0)
ap.add_argument("--window", type=int, nargs=2, metavar=("LO", "HI"))
args = ap.parse_args()

cfg = default(); m = cfg.machine; spd = m.a.steps_per_unit
profile = PEN if args.pen else KNIFE
tang = profile.tangential

subs, _ = load_svg_mm_subpaths(os.path.join(base, args.svg))
rep = [enforce_c1(sp, cfg.quality.angle_tol, cfg.quality.gap_tol)[0] for sp in subs]
flat = [c for sp in rep for c in sp]
flags = []
for sp in rep:
    for i in range(len(sp)):
        f = PATH_START if i == 0 else 0
        if i == len(sp) - 1: f |= PATH_END
        flags.append(f)
metrics = compute_metrics(flat)
planned = plan_velocities(metrics, flags, args.feed, args.amax,
                          corner_stop_angle_deg=(profile.corner_angle_deg if tang else None),
                          a_rate_deg_s=(m.a.max_rate if tang else 0.0))
segs = build_toolpath(planned, m, profile=profile, lift_height=3.0,
                      jog_feed=args.feed, z_feed=8, a_max=args.amax)

def dur_ms(s):
    major = max(abs(s.dx), abs(s.dy), abs(s.dz), abs(s.da))
    return s.interval * major / (m.f_cpu / 1e6) / 1000

print(f"tool={profile.name}  segments={len(segs)}  "
      f"total~{sum(dur_ms(s) for s in segs)/1000:.1f}s")

# in-curve cusps
theta = 0.0; px = py = 0.0
cusp_angles = []
for p in planned:
    if p.flags & PATH_START:
        theta = _tangent_angle(p.metrics.curve, 0.0); px = py = 0.0
    _, theta, px, py, cusps = evaluate_curve(
        p, m, cfg.quality, theta, px, py, bool(p.flags & PATH_END), tang,
        corner_angle_deg=(profile.corner_angle_deg if tang else 360.0))
    cusp_angles += [pivot / spd for _, pivot in cusps]
print(f"in-curve cusps: {len(cusp_angles)}  angles(deg)="
      f"{[f'{a:.0f}' for a in cusp_angles]}")

bad = [(i, s) for i, s in enumerate(segs)
       if not (s.flags & (MICRO_JOG | MICRO_LIFT)) and abs(s.da) > 20 * spd]
print(f"pen-down big-da segments (tear risk): {len(bad)}")

# cumulative physical A winding (true degrees) -- peak |wind| = wire twist
inv = -1 if m.a.invert else 1
wind = 0.0; peak = 0.0
for s in segs:
    wind += s.da * inv / spd
    peak = max(peak, abs(wind))
print(f"physical A: end={wind:.0f}deg  peak|wind|={peak:.0f}deg")

if args.window:
    lo, hi = args.window
    for i in range(lo, min(hi, len(segs))):
        s = segs[i]
        tag = ",".join(t for t, b in (("JOG", s.flags & MICRO_JOG),
                                      ("LIFT", s.flags & MICRO_LIFT),
                                      ("END", s.flags & MICRO_PATH_END)) if b)
        print(f"  {i:5d}: {dur_ms(s):7.1f}ms dx={s.dx:4d} dy={s.dy:4d} "
              f"dz={s.dz:5d} da={s.da:5d} iv={s.interval:9d} {tag}")
