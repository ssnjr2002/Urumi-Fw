"""
microsegment.py — the MicroSegment wire-event type and its emit helpers.

The bottom-of-pipeline unit: one MicroSegment is one step-timing event (per-axis
integer step deltas + a clock interval). The Discretize stage produces these;
host/serialise.py packs them to the 26-byte wire format. Moved out of the
tile-era stage6 so it survives that file's deletion — the type and the interval
math are engine-neutral.
"""

import math
from collections import namedtuple

MicroSegment = namedtuple("MicroSegment", [
    "dx",        # X steps (signed int)
    "dy",        # Y steps (signed int)
    "dz",        # Z steps (signed int)
    "da",        # A steps (signed int, tangential rotation)
    "interval",  # clock cycles for major axis
    "flags",     # MICRO_PATH_END etc.
])

# The flags byte is ONE namespace shared with the wire (see docs/wire_protocol.md).
# Low bits are wire/firmware semantics, high bits are host planning hints the
# firmware ignores:
#   0x01 PATH_END (shared)   0x02 ESTOP (wire)   0x04 PAUSE (wire, sender-inserted)
#   0x08 LIFT (host hint)    0x10 JOG (host hint)
# JOG must NOT be 0x04 — that would alias every travel move onto MSEG_FLAG_PAUSE.
MICRO_PATH_END = 0x01
MICRO_LIFT     = 0x08   # pen/tool Z raise or lower (host hint)
MICRO_JOG      = 0x10   # travel move between subpaths (host hint)


def angle_delta(a, b):
    """Shortest signed rotation from angle a to angle b (degrees, range ±180)."""
    d = b - a
    while d >  180: d -= 360
    while d < -180: d += 360
    return d


def interval(v, machine, q, dx=None, dy=None, dz=0, da=0):
    """
    Clock cycles per major-axis step so the XY TOOL moves at v mm/s.

    The Pico times a segment by its major axis (max steps over all driven axes),
    but the tool travels the XY hypotenuse — longer than the major leg on a
    diagonal. Without correction the realized tool speed overshoots v by up to
    sqrt(2). Scaling the interval by hypot(dx,dy)/major restores the commanded
    feed; for a pure axis move hypot == major and it reduces to the plain
    major-axis rate.

    Called without dx/dy it governs the major axis directly at v (legacy path).

    Per-axis: the XY tool distance is hypot(dx/x_spu, dy/y_spu), so X and Y may
    have different resolutions (non-square machine). A per-axis rate limit floors
    the segment time so no axis exceeds max_rate_i * steps_per_unit_i — this is
    what keeps the A axis within its slew rate on tight curves.
    """
    v = max(v, q.v_min)
    x_spu = machine.x.steps_per_unit
    y_spu = machine.y.steps_per_unit

    def _major_rate():
        step_rate = v * x_spu
        if step_rate < 1e-6:
            return machine.f_cpu
        return max(1, min(int(machine.f_cpu / step_rate), machine.f_cpu))

    if dx is None or dy is None:
        return _major_rate()

    major = max(abs(dx), abs(dy), abs(dz), abs(da))
    if major == 0:
        return machine.f_cpu

    t_rate = 0.0
    for d, ax in ((dx, machine.x), (dy, machine.y), (dz, machine.z), (da, machine.a)):
        R = ax.max_rate * ax.steps_per_unit
        if R > 0 and d != 0:
            t_rate = max(t_rate, abs(d) / R)

    dist_mm = math.hypot(dx / x_spu, dy / y_spu)   # true XY tool distance (mm)
    if dist_mm < 1e-9:
        # pure rotation / Z move — no XY feed to govern; use the rate floor if any
        if t_rate > 0.0:
            cycles = t_rate / major * machine.f_cpu
            return max(1, min(int(cycles), machine.f_cpu))
        return _major_rate()

    seg_time = max(dist_mm / v, t_rate)        # feed time, floored by axis rates
    cycles = seg_time / major * machine.f_cpu  # per major-axis step
    return max(1, min(int(cycles), machine.f_cpu))
