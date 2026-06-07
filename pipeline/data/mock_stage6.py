"""
Mock input data for stage 6 (Bezier evaluation -> MicroSegments).
Each case provides:
  - planned:      list of PlannedCurve (from stage 5)
  - machine:      MachineConfig (steps_per_mm, steps_per_degree, f_cpu)
  - expected:     dict of assertions the tests will check
"""

import sys, os, math
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "stages"))
from stage1 import CubicBezier
from stage4 import CurveMetrics, kappa_samples, arc_length
from stage5 import PlannedCurve, PATH_START, PATH_END, MERGE_WITH_PREV
from collections import namedtuple

# ── machine config ────────────────────────────────────────────────────────────

MachineConfig = namedtuple("MachineConfig", [
    "steps_per_mm",     # XY resolution
    "steps_per_deg",    # A axis resolution
    "f_cpu",            # RP2350 clock Hz (for interval computation)
])

MACHINE_DEFAULT = MachineConfig(
    steps_per_mm=80,
    steps_per_deg=10,
    f_cpu=150_000_000,
)

# ── helpers ───────────────────────────────────────────────────────────────────

def pt(x, y):
    return (float(x), float(y))

def make_planned(p0, p1, p2, p3, v_entry, v_cruise, v_exit, flags=0):
    c = CubicBezier(pt(*p0), pt(*p1), pt(*p2), pt(*p3))
    samples = kappa_samples(c)
    m = CurveMetrics(
        curve=c,
        path_length_mm=arc_length(c),
        kappa_max=max(k for _, k in samples),
        kappa_samples=samples,
    )
    return PlannedCurve(metrics=m, flags=flags, v_entry=v_entry,
                        v_cruise=v_cruise, v_exit=v_exit, merged=False)

def straight_planned(x0, x1, y=0.0, v_entry=50.0, v_cruise=50.0, v_exit=50.0, flags=0):
    dx = (x1 - x0) / 3
    return make_planned((x0,y),(x0+dx,y),(x1-dx,y),(x1,y),
                        v_entry, v_cruise, v_exit, flags)

def arc_planned(r, v_entry, v_cruise, v_exit, flags=0):
    h = r * 0.5522847
    return make_planned((r,0),(r,h),(h,r),(0,r),
                        v_entry, v_cruise, v_exit, flags)

# ── case 1: pure horizontal line, constant velocity ───────────────────────────
# dx only, dy=0, da=0. Interval should be constant.
# At v=50mm/s, steps_per_mm=80: step_rate=4000 steps/s
# interval = f_cpu / step_rate = 150e6 / 4000 = 37500 cycles

STRAIGHT_CONSTANT_V = dict(
    planned=[straight_planned(0, 100, v_entry=50, v_cruise=50, v_exit=50,
                              flags=PATH_START | PATH_END)],
    machine=MACHINE_DEFAULT,
    expected={
        "all_dy_zero": True,
        "all_da_zero": True,
        "interval_approx": 150_000_000 // (50 * 80),
    },
)

# ── case 2: diagonal line 45°, constant velocity ─────────────────────────────
# dx ≈ dy throughout. da=0 (tangent constant at 45°).

DIAGONAL_CONSTANT_V = dict(
    planned=[make_planned((0,0),(33.33,33.33),(66.67,66.67),(100,100),
                          50, 50, 50, PATH_START | PATH_END)],
    machine=MACHINE_DEFAULT,
    expected={
        "dx_approx_dy": True,
        "all_da_zero": True,
    },
)

# ── case 3: accelerating straight line ───────────────────────────────────────
# v_entry=0 → v_exit=100. Velocity subdivision constraint should produce
# finer MicroSegments near start (slow) and coarser near end (fast).
# Total step count = length * steps_per_mm = 50 * 80 = 4000 steps.

ACCEL_STRAIGHT = dict(
    planned=[straight_planned(0, 50, v_entry=0, v_cruise=100, v_exit=100,
                              flags=PATH_START)],
    machine=MACHINE_DEFAULT,
    expected={
        "total_steps_approx": 50 * 80,
        "intervals_decrease": True,   # intervals get shorter as speed increases
    },
)

# ── case 4: quarter circle with tangent tracking ──────────────────────────────
# Tangent rotates 90° from 0° to 90°. da should accumulate ~90 * steps_per_deg.
# dx goes from positive to zero; dy goes from zero to positive.

ARC_TANGENT_TRACKING = dict(
    planned=[arc_planned(50, v_entry=50, v_cruise=50, v_exit=50,
                         flags=PATH_START | PATH_END)],
    machine=MACHINE_DEFAULT,
    expected={
        "total_da_approx": 90 * MACHINE_DEFAULT.steps_per_deg,
        "net_dx_approx": 50 * 80,    # net X displacement = radius * steps_per_mm
        "net_dy_approx": 50 * 80,    # net Y displacement = radius * steps_per_mm
    },
)

# ── case 5: tight arc — geometric subdivision dominates ───────────────────────
# r=5mm, high curvature → |B''| large → dt_geom is very small → many segments.
# Velocity subdivision irrelevant (constant v).

TIGHT_ARC_DENSE = dict(
    planned=[arc_planned(5, v_entry=30, v_cruise=30, v_exit=30,
                         flags=PATH_START | PATH_END)],
    machine=MACHINE_DEFAULT,
    expected={
        "min_segment_count": 20,   # should produce many segments due to curvature
    },
)

# ── case 6: near-zero velocity (v_min clamp) ─────────────────────────────────
# v_entry=0 at PATH_START. Stage 6 must clamp to v_min, not divide by zero.
# interval must be finite and <= some sane max.

NEAR_ZERO_VELOCITY = dict(
    planned=[straight_planned(0, 10, v_entry=0, v_cruise=0.5, v_exit=0.5,
                              flags=PATH_START)],
    machine=MACHINE_DEFAULT,
    expected={
        "no_infinite_interval": True,
        "interval_max": 150_000_000 // 1,  # at most 1 step/sec = 150M cycles
    },
)

# ── case 7: deceleration to stop ──────────────────────────────────────────────
# v_entry=100, v_exit=0 at PATH_END. Intervals should increase as machine slows.
# Last MicroSegment before stop should have largest interval.

DECEL_TO_STOP = dict(
    planned=[straight_planned(0, 50, v_entry=100, v_cruise=100, v_exit=0,
                              flags=PATH_END)],
    machine=MACHINE_DEFAULT,
    expected={
        "intervals_increase": True,
        "last_interval_largest": True,
    },
)

# ── case 8: two-curve chain — position and angle continuous ───────────────────
# Curve[0] exits rightward; curve[1] continues rightward.
# Step position must be continuous across the boundary (no gap).

TWO_CURVE_CHAIN = dict(
    planned=[
        straight_planned(0,   80, v_entry=0,   v_cruise=80, v_exit=80,  flags=PATH_START),
        straight_planned(80, 160, v_entry=80,  v_cruise=80, v_exit=0,   flags=PATH_END),
    ],
    machine=MACHINE_DEFAULT,
    expected={
        "position_continuous": True,
        "total_steps_approx": 160 * 80,
    },
)

# ── registry ──────────────────────────────────────────────────────────────────

CASES = {
    "straight_constant_v":   STRAIGHT_CONSTANT_V,
    "diagonal_constant_v":   DIAGONAL_CONSTANT_V,
    "accel_straight":        ACCEL_STRAIGHT,
    "arc_tangent_tracking":  ARC_TANGENT_TRACKING,
    "tight_arc_dense":       TIGHT_ARC_DENSE,
    "near_zero_velocity":    NEAR_ZERO_VELOCITY,
    "decel_to_stop":         DECEL_TO_STOP,
    "two_curve_chain":       TWO_CURVE_CHAIN,
}

if __name__ == "__main__":
    for name, case in CASES.items():
        planned = case["planned"]
        machine = case["machine"]
        print(f"{name}  ({len(planned)} curve(s))  "
              f"steps/mm={machine.steps_per_mm}  f_cpu={machine.f_cpu/1e6:.0f}MHz")
        for i, p in enumerate(planned):
            print(f"  [{i}] length={p.metrics.path_length_mm:.2f}mm  "
                  f"kappa_max={p.metrics.kappa_max:.4f}  "
                  f"v={p.v_entry:.1f}->{p.v_cruise:.1f}->{p.v_exit:.1f} mm/s")
        print(f"  expected: {case['expected']}")
        print()
