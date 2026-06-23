"""
Sample — the spine of the redesigned pipeline (stages 4-8).

A Sample is one point along a flattened toolpath: position, tangent, local
curvature, and the arc-length step to the NEXT sample. After the Flatten stage
(flatten.py) the whole job is a single flat list[Sample]; every downstream stage
operates on that list instead of on Bezier tiles. See PLAN_pipeline_redesign.md
for why the planning unit is the arc-length sample, not the Bezier curve.

The planning fields (v_ceiling, v) start unset and are filled in place by the
Constrain and Plan stages — a Sample is mutable on purpose, it accretes a
velocity as it moves down the pipeline.

Angle convention: theta is in DEGREES (matches steps_per_deg and the existing
tangent helpers); curvature kappa is in 1/mm (a geometric quantity, angle-unit
independent). The A-slew velocity cap converts deg/s -> rad/s where it uses kappa.
"""

from dataclasses import dataclass, field

# ── provenance flags ──────────────────────────────────────────────────────────
# These mark WHERE a sample came from; the tool-dependent CORNER decision (is a
# curve-boundary tangent jump sharp enough to lift-pivot?) is made downstream
# where the ToolProfile is known, not here.

PATH_START     = 0x01   # first sample of a subpath (planner forces v_entry = 0)
PATH_END       = 0x02   # last sample of a subpath  (planner forces v_exit  = 0)
CURVE_BOUNDARY = 0x04   # first sample of a curve that follows another in the
                        # same subpath. The previous sample is the prior curve's
                        # t=1; the two share a position but may differ in tangent
                        # — that tangent difference IS the corner signal.


@dataclass
class Sample:
    x:     float                 # mm, machine frame
    y:     float                 # mm, machine frame
    theta: float                 # tangent angle, degrees (unwrapped within subpath)
    kappa: float                 # local curvature, 1/mm (>= 0)
    ds:    float                 # arc length from this sample to the next, mm
                                 # (0.0 at the last sample of a subpath, and ~0 at
                                 # a shared curve boundary)
    flags: int = 0               # PATH_START | PATH_END | CURVE_BOUNDARY

    # ── filled downstream ─────────────────────────────────────────────────────
    v_ceiling: float = field(default=float("inf"))  # Constrain: local speed cap
    v:         float = field(default=0.0)            # Plan: resolved speed (mm/s)
