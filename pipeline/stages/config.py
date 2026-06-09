"""
pipeline/config.py — single source of truth for all pipeline parameters.

The stages stay pure functions that receive these values explicitly; this
module is only where the values are DEFINED. Standalone stage CLIs read their
argparse defaults from default() and still allow per-flag overrides, so every
stage remains independently runnable and debuggable.

Three tiers, mirroring the host/local-production split:

  machine — calibration / controller-physical. MUST match the Pico when
            microsegment generation runs locally on the RP2350 (steps_per_mm
            is derived from the node's driver microstepping, so it is not the
            host's to invent — it is shared and must agree).
  motion  — kinematic limits (feed / acceleration / jog).
  quality — algorithm tuning. This tier is the parity spec the future C++ port
            must reproduce to emit identical microsegments.

TOML loading (load()) is intentionally deferred — default() is the only source
for now.
"""

from dataclasses import dataclass, field


@dataclass(frozen=True)
class MachineConfig:
    """Calibration / controller-physical. Must agree with the Pico firmware."""
    steps_per_mm:  float = 80.0
    steps_per_deg: float = 10.0
    f_cpu:         int   = 150_000_000   # RP2350 clock Hz — the domain `interval` is expressed in


@dataclass(frozen=True)
class MotionConfig:
    """Kinematic limits."""
    feed_max: float = 80.0      # mm/s — cruise ceiling
    a_max:    float = 1000.0    # mm/s^2
    jog_feed: float = 80.0      # mm/s — travel between subpaths


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


@dataclass(frozen=True)
class PipelineConfig:
    machine: MachineConfig = field(default_factory=MachineConfig)
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
