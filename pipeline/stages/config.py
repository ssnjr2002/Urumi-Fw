"""
pipeline/config.py — single source of truth for all pipeline parameters.

The stages stay pure functions that receive these values explicitly; this
module is only where the values are DEFINED. Standalone stage CLIs read their
argparse defaults from default() and still allow per-flag overrides, so every
stage remains independently runnable and debuggable.

Three tiers, mirroring the host/local-production split:

  machine — calibration / controller-physical, now PER-AXIS. MUST match the
            Pico when microsegment generation runs locally on the RP2350.
  motion  — kinematic limits (feed / acceleration / jog).
  quality — algorithm tuning. This tier is the parity spec the future C++ port
            must reproduce to emit identical microsegments.

TOML loading (load()) is intentionally deferred — default() is the only source
for now.
"""

from dataclasses import dataclass, field


# ── machine tier (per-axis) ───────────────────────────────────────────────────

@dataclass(frozen=True)
class AxisConfig:
    """
    One physical axis.

    steps_per_unit is steps per mm for a linear axis, or steps per degree for a
    rotary axis (rotary=True) — the same convention FluidNC uses when it treats
    a rotary A axis as "mm" of degrees.

    node binds this axis to the ATtiny that physically drives it — this is the
    axis->node map that previously lived only as prose in the PLAN docs.

    max_rate / accel / max_travel describe the axis's physical capability and
    work envelope. They are the eventual source for per-axis velocity planning
    and soft-limit (envelope) checks.
    NOTE: the current stage5/stage6 pipeline still plans with the SCALAR
    MotionConfig.{feed_max,a_max}; these per-axis limits are carried by the
    config but not yet consumed. See the MachineConfig scalar-bridge note.
    """
    node:           int            # ATtiny node id (1-4) driving this axis
    steps_per_unit: float          # steps/mm (linear) or steps/deg (rotary)
    max_rate:       float = 0.0    # units/s   — physical ceiling (not yet consumed)
    accel:          float = 0.0    # units/s^2 — physical ceiling (not yet consumed)
    max_travel:     float = 0.0    # units     — envelope extent, 0 = unset (not yet consumed)
    invert:         bool  = False  # flip commanded direction for this axis
    rotary:         bool  = False  # True for the tangential A axis
    present:        bool  = True   # False = axis not fitted (its deltas are 0)


@dataclass(frozen=True)
class MachineConfig:
    """
    Per-axis machine definition — the single source of truth for axis
    calibration, the axis->node map, and (eventually) per-axis limits. Must
    agree with the Pico firmware.

    Scalar bridge
    ─────────────
    The current pipeline (stage5/stage6) is still scalar. Until it is rewired
    for per-axis resolution, the steps_per_mm / steps_per_deg properties expose
    the legacy scalar view. steps_per_mm asserts X and Y agree and raises
    otherwise — so a non-square machine fails LOUDLY instead of silently cutting
    at the wrong scale. Remove the bridge once stage5/6 consume AxisConfig
    directly.
    """
    x: AxisConfig
    y: AxisConfig
    z: AxisConfig
    a: AxisConfig
    f_cpu: int = 150_000_000   # RP2350 clock Hz — the domain `interval` is expressed in

    @classmethod
    def uniform(cls, steps_per_mm, steps_per_deg, f_cpu=150_000_000,
                max_rate=80.0, accel=1000.0):
        """
        Build an equal-XY (single belt/pulley) machine using the conventional
        X=node1, Y=node2, Z=node3, A=node4 map. Z defaults to not-present since
        the pipeline does not drive Z yet (dz=0). Convenience for the common
        simple case and for callers that only carry the three legacy scalars.
        """
        return cls(
            x=AxisConfig(node=1, steps_per_unit=steps_per_mm,  max_rate=max_rate, accel=accel),
            y=AxisConfig(node=2, steps_per_unit=steps_per_mm,  max_rate=max_rate, accel=accel),
            z=AxisConfig(node=3, steps_per_unit=steps_per_mm,  max_rate=max_rate, accel=accel,
                         present=False),
            a=AxisConfig(node=4, steps_per_unit=steps_per_deg, max_rate=max_rate, accel=accel,
                         rotary=True),
            f_cpu=f_cpu,
        )

    # ── scalar bridge — delete when stage5/6 go per-axis ──────────────────────

    @property
    def steps_per_mm(self) -> float:
        if self.x.steps_per_unit != self.y.steps_per_unit:
            raise ValueError(
                "X and Y steps_per_unit differ — the pipeline is still scalar. "
                "Rewire stage5/stage6 for per-axis resolution before driving a "
                "non-square machine."
            )
        return self.x.steps_per_unit

    @property
    def steps_per_deg(self) -> float:
        return self.a.steps_per_unit


# ── motion + quality tiers ────────────────────────────────────────────────────

@dataclass(frozen=True)
class MotionConfig:
    """
    Kinematic limits the SCALAR planner currently consumes.
    When stage5 goes per-axis, a_max is derived from AxisConfig and retires here;
    feed_max becomes the default programmed feed only.
    """
    feed_max: float = 80.0      # mm/s — cruise ceiling
    a_max:    float = 1000.0    # mm/s^2
    jog_feed: float = 80.0      # mm/s — travel between subpaths
    junction_deviation: float = 0.05   # mm — max corner rounding for junction-deviation cornering
    lift_height: float = 0.0    # mm — pen/tool lift between subpaths (0 = no lift, draw-through)
    z_feed:      float = 20.0   # mm/s — Z raise/lower speed


@dataclass(frozen=True)
class QualityConfig:
    """Algorithm tuning — the parity spec the C++ port must reproduce."""
    chord_tol: float = 0.01     # mm — max chord deviation per segment (stage 6)
    dv_max:    float = 3.0      # mm/s — max velocity change per segment (stage 6)
    v_min:     float = 0.5      # mm/s — velocity floor, avoids divide-by-zero (stage 6)
    dt_max:    float = 0.05     # max parameter step (stage 6)
    dt_min:    float = 1e-6     # loop guard (stage 6)
    angle_tol: float = 5.0      # deg — C1 continuity tolerance (stage 3)
    gap_tol:   float = 0.01     # mm — join gap tolerance (stage 3)
    n_kappa:   int   = 20       # curvature samples per curve (stage 4)


def _default_machine() -> MachineConfig:
    """
    The physical machine: DM542 @ 1/32 microstepping.
      X/Y : GT2 20T pulley, 40 mm/rev -> 160 steps/mm
      Z   : lead screw -> 1200 steps/mm
      A   : tangential rotary -> 120 steps/deg
    Node map X=1, Y=2, Z=3, A=4. All four axes fitted (present).

    Per-axis max_rate/accel are PROVISIONAL: X/Y mirror the scalar motion limits
    the pipeline still uses; Z/A are left 0 (uncharacterised) until per-axis
    planning consumes them. NOTE: Z at 1200 steps/mm is slow mechanically — drive
    it at a low feed (the jog tool's --feed is units/s, so a few mm/s for Z).
    """
    # ── PLACEHOLDER max_rate for Z and A — MEASURE AND REPLACE ────────────────
    # Per-axis rate limiting (stage6._interval) slows a segment so no axis
    # exceeds max_rate * steps_per_unit. A value of 0 means "unlimited". The
    # numbers below are conservative guesses so the limiter is active; replace
    # with the real physical ceilings once characterised:
    #   a.max_rate — how fast the tangential knife can actually slew (deg/s)
    #   z.max_rate — Z raise/lower ceiling (mm/s); 1200 steps/mm is slow, keep low
    return MachineConfig(
        x=AxisConfig(node=1, steps_per_unit=160.0,  max_rate=80.0, accel=1000.0, invert=True),
        y=AxisConfig(node=2, steps_per_unit=160.0,  max_rate=80.0, accel=1000.0),
        z=AxisConfig(node=3, steps_per_unit=1200.0, max_rate=10.0, invert=True),    # PLACEHOLDER mm/s
        a=AxisConfig(node=4, steps_per_unit=120.0, rotary=True, max_rate=360.0),  # PLACEHOLDER deg/s
    )


@dataclass(frozen=True)
class PipelineConfig:
    machine: MachineConfig = field(default_factory=_default_machine)
    motion:  MotionConfig  = field(default_factory=MotionConfig)
    quality: QualityConfig = field(default_factory=QualityConfig)


def default() -> PipelineConfig:
    """The single set of defaults. Every stage CLI sources its defaults here."""
    return PipelineConfig()


def load(path) -> PipelineConfig:
    """Load config from a TOML file. Deferred — not implemented yet."""
    raise NotImplementedError(
        "TOML config loading is not implemented yet; use config.default()"
    )
