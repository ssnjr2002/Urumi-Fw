"""
pipeline/config.py — single source of truth for all pipeline parameters.

The stages stay pure functions that receive these values explicitly; this
module is only where the values are DEFINED. Standalone stage CLIs read their
argparse defaults from default() and still allow per-flag overrides, so every
stage remains independently runnable and debuggable.

Three tiers, mirroring the host/local-production split:

  machine — calibration / controller-physical, now PER-AXIS. MUST match the
            Pico when microsegment generation runs locally on the RP2350. Also
            carries machine-level travel defaults (jog_feed, z_feed).
  quality — algorithm tuning. This tier is the parity spec the future C++ port
            must reproduce to emit identical microsegments. Carries
            junction_deviation (corner-rounding budget).
  tool    — ToolProfile: per-tool kinematic behaviour AND the programmed cut
            feed (feed_max). The tool is a TARGET source; physical ceilings live
            on AxisConfig (max_rate/accel) and are enforced regardless.

Bus-first topology
──────────────────
The RS485 bus has NODES (ATtiny3224 driver boards). A node can be anything —
a stepper axis, an oscillating-knife controller, a suction valve. `BusNode`
is the primitive: it captures what is physically on the bus (id, role,
present/absent). Things that USE a node reference it:

  AxisConfig  → a motion axis; holds a BusNode plus the step-math that turns
                mm/deg into steps. (An axis is a node WITH calibration.)
  ToolHead    → a co-mounted Z + A pair plus the tool mounted on it and its
                X mounting offset. A machine has one or more heads; only one
                is active at a time (dual heads are side-by-side, software
                selected, never simultaneous).
  peripherals → non-axis BusNodes (knife controller, suction) — present for
                topology completeness; not yet consumed by the pipeline.

The current machine is a single centred head (x_offset = 0), so `machine.z`
and `machine.a` resolve to that one head and the pipeline is unchanged. A
future dual-head machine just adds a second ToolHead with a real x_offset and
its own Z/A nodes; switching is `replace(machine, active_head=i)`.

MotionConfig retired: feed_max → ToolProfile (cut-feed target, not a limit);
jog_feed/z_feed → MachineConfig (machine-level travel defaults);
junction_deviation → QualityConfig; a_max → AxisConfig.accel (per-axis, the XY
plane accel — X and Y share the value).

TOML loading (load()) is intentionally deferred — default() is the only source
for now.
"""

from dataclasses import dataclass, field


# ── bus tier (the RS485 topology primitive) ───────────────────────────────────

@dataclass(frozen=True)
class BusNode:
    """
    One ATtiny3224 driver board on the RS485 bus.

    node_id is the RS485 address (currently 1-4; the 4-node ceiling moves when
    dual heads + non-axis nodes land). role names what the board drives —
    "stepper" for an axis, "oscillator" for the knife controller, "suction" for
    the vacuum, etc. present = False marks a node declared in the topology but
    not physically fitted (its axis deltas are 0 / its peripheral is ignored).
    """
    node_id: int
    role:    str  = "stepper"
    present: bool = True


# ── machine tier (per-axis) ───────────────────────────────────────────────────

@dataclass(frozen=True)
class AxisConfig:
    """
    One physical motion axis — a BusNode WITH the step-math to drive it.

    steps_per_unit is steps per mm for a linear axis, or steps per degree for a
    rotary axis (rotary=True) — the same convention FluidNC uses when it treats
    a rotary A axis as "mm" of degrees.

    node is the BusNode this axis drives — the axis->node binding that
    previously lived only as prose in the PLAN docs. The RS485 wire address is
    `node.node_id`.

    max_rate / accel / max_travel describe the axis's physical capability and
    work envelope. accel is the per-axis acceleration the planner ramps with
    (the XY value is what the old scalar a_max held); max_rate is the physical
    velocity ceiling that clamps every programmed feed (cut feed_max and travel
    jog_feed alike) — so a slow Z stays within its limit while XY run faster.
    max_travel feeds soft-limit (envelope) checks.
    """
    node:           BusNode        # the bus node this axis drives
    steps_per_unit: float          # steps/mm (linear) or steps/deg (rotary)
    max_rate:       float = 0.0    # units/s   — physical ceiling (not yet consumed)
    accel:          float = 0.0    # units/s^2 — physical ceiling (not yet consumed)
    max_travel:     float = 0.0    # units     — envelope extent, 0 = unset (not yet consumed)
    invert:         bool  = False  # flip commanded direction for this axis
    rotary:         bool  = False  # True for the tangential A axis


# ── tool tier ─────────────────────────────────────────────────────────────────

# Blade offset below this (mm) is treated as a centre-pivot tangential tool and
# run uncompensated — the worst-case error is ~offset, concentrated at corners,
# and below this it sits within real cut tolerance (~0.1 mm). Above it, offset
# compensation (XY_pivot = XY_cut - offset * tangent; PLAN_svg_tile_motion P6)
# is required and not implemented yet — build_toolpath raises rather than
# silently cutting wrong. Raise this only once compensation exists.
OFFSET_TOLERANCE_MM = 0.05


@dataclass(frozen=True)
class ToolProfile:
    """
    One mounted tool's kinematic behaviour. The choreography in stage6
    (build_toolpath) reads this to decide tangent tracking, lift, and corner
    handling — so a new tool is a new preset here, never a code change.

    There is ONE knife model, not two. "Tangential vs drag" is not a tool type;
    on a driven-A machine the blade is always actively oriented, and the only
    difference is the blade's caster offset (offset_mm) — a single parameter.
    offset_mm = 0 is the clean centre-pivot case; a larger offset would need
    offset compensation (PLAN_svg_tile_motion P6), the SAME formula with the
    offset as a term (identity at 0). Until that lands, offset_mm above
    OFFSET_TOLERANCE_MM is rejected by build_toolpath. Tool TYPES (pen / cut /
    crease) are the real distinctions; see PLAN_svg_tile_motion tool table.

    Designed so every tool-specific behaviour degrades to a no-op at its
    zero/off value, so pen/crease/knife share one code path with no special
    casing.

    corner_* / min_radius_mm tune corner handling (lift-pivot-lower).
    """
    name:            str
    tangential:      bool  = False   # A-axis tracks the path tangent
    offset_mm:       float = 0.0     # blade caster offset; 0 = centre-pivot, >tol needs compensation
    unwind:          bool  = False   # bounded rotation (e.g. wired tool): unwind
                                     # accumulated full turns during pen-up moves
                                     # so the cable never twists past ~one turn.
                                     # False = free-spinning tool (crease wheel).
    corner_angle_deg: float = 20.0   # tangent jump above which a corner action fires
    min_radius_mm:   float = 0.0     # curvature floor; tighter arcs need special handling (0 = unset)
    feed_max:        float = 80.0    # mm/s — programmed cut feed (TARGET, not a limit;
                                     # AxisConfig.max_rate still clamps it per axis)
    lift_height:     float = 0.0     # Z lift between subpaths, mm (0 = draw-through)
    z_feed:          float = 0.0     # Z raise/lower speed, mm/s (0 = use MachineConfig.z_feed)
    jog_feed:        float = 0.0     # travel speed between subpaths, mm/s (0 = use MachineConfig.jog_feed)
    required_peripheral_roles: tuple = ()  # bus-node roles this tool needs present
                                     # and responsive (e.g. ("oscillator",) for the
                                     # driven knife). Declared by ROLE, not node id,
                                     # so the profile stays machine-agnostic;
                                     # pre-flight resolves against machine.peripherals.

    @property
    def needs_offset_comp(self) -> bool:
        """True if the offset is large enough to require (unimplemented) compensation."""
        return self.offset_mm > OFFSET_TOLERANCE_MM

    @property
    def required_axes(self) -> int:
        """
        Axis bitmask (bit0=X bit1=Y bit2=Z bit3=A) that must be homed before a job
        with this tool is accepted. X and Y are always required; Z only if the tool
        lifts; A only if it tracks the tangent. Derived from existing fields — no
        redundant stored value. Used by the pre-flight homed check and the resume
        gate (see docs/state_redesign.md).
        """
        mask = 0b0011                       # X, Y always
        if self.lift_height > 0:
            mask |= 0b0100                  # Z — tool lift
        if self.tangential:
            mask |= 0b1000                  # A — tangent tracking
        return mask


# Presets keyed to tool TYPE (PLAN_svg_tile_motion: pen/cut/crease). One knife
# model (KNIFE); a larger-offset blade just sets offset_mm, not a new profile.
PEN = ToolProfile(name="pen", tangential=False)

KNIFE = ToolProfile(
    name="knife", tangential=True, offset_mm=0.0,   # centre-pivot; raise offset_mm per blade
    unwind=True,                                     # oscillating knife is wired
    corner_angle_deg=20.0,
    # required_peripheral_roles left empty: today's machine drives the blade via
    # the A stepper (node 4); a SEPARATE oscillator-controller node is future
    # hardware. Add ("oscillator",) here once that node exists in machine.peripherals.
)

CREASE = ToolProfile(
    name="crease", tangential=True, offset_mm=0.0,
    unwind=False,                                    # crease wheel spins freely
    corner_angle_deg=30.0,
)

TOOL_PROFILES = {p.name: p for p in (PEN, KNIFE, CREASE)}


def tool_for_layer(layer_name, overrides=None):
    """
    Resolve an SVG layer name to a ToolProfile. Convention: the layer label
    matches a tool name (pen / knife / crease), case-insensitive — so an Inkscape
    "knife" layer cuts with KNIFE. `overrides` is an optional {layer_name: profile}
    map for names that don't follow the convention. Returns None if unresolved
    (caller decides whether that layer is skipped or an error).
    """
    name = (layer_name or "").strip().lower()
    if overrides:
        for k, v in overrides.items():
            if k.strip().lower() == name:
                return v
    return TOOL_PROFILES.get(name)


# ── head tier (a Z+A pair + the tool mounted on it) ───────────────────────────

@dataclass(frozen=True)
class ToolHead:
    """
    One physical tool head: a co-mounted Z + A pair, the tool mounted on it,
    and its X mounting offset.

    A machine has one or more heads. On the current machine there is a single
    centred head (x_offset = 0). A dual-head machine fixes two heads side by
    side along X; they are software-selected, NEVER run simultaneously, so only
    one head's Z/A are "live" at a time (see MachineConfig.active_head).

    profile is the tool currently mounted on this head (PEN/KNIFE/CREASE) — this
    is how the planner knows which head carries which tool: to run a knife job
    it selects the head whose profile is KNIFE. (Auto-selection from the head's
    profile is a follow-up; today the stages still take an explicit profile and
    this field records the topology.)

    x_offset is the head's X position relative to the machine X reference (0 =
    centred). When a non-centred head is active every XY move must be corrected
    by this offset before stepping — not yet consumed (single centred head).
    """
    z:        AxisConfig
    a:        AxisConfig
    profile:  ToolProfile = PEN
    x_offset: float       = 0.0    # mm from machine X reference (not yet consumed)


@dataclass(frozen=True)
class MachineConfig:
    """
    Per-machine definition — the single source of truth for axis calibration,
    the axis->node bindings, the tool heads, and the bus topology. Must agree
    with the Pico firmware.

    X and Y are the shared gantry (one pair, all heads use them). Z and A are
    per-head: `machine.z` / `machine.a` resolve to the ACTIVE head, so the
    pipeline reads them exactly as before and a single-head machine behaves
    identically to the old flat x/y/z/a layout.

    The whole pipeline now consumes per-axis fields directly
    (machine.x.steps_per_unit, machine.a.accel, ...) — the old scalar bridge
    (steps_per_mm / steps_per_deg properties) is retired. The only scalar entry
    point left is uniform() below, an explicit square-machine builder.
    """
    x: AxisConfig
    y: AxisConfig
    heads: tuple                           # tuple[ToolHead, ...]; [0] = primary
    active_head: int = 0                   # which head's Z/A are live
    f_cpu: int = 150_000_000               # RP2350 clock Hz — `interval`'s domain
    # Machine-level travel defaults (a ToolProfile may override per tool). These
    # are TARGETS; AxisConfig.max_rate still clamps them per axis.
    jog_feed: float = 80.0                 # mm/s — XY travel speed between subpaths
    z_feed:   float = 20.0                 # mm/s — Z raise/lower speed
    # Non-axis nodes on the bus (knife controller, suction). Topology only —
    # not yet consumed by the pipeline.
    peripherals: tuple = ()                # tuple[BusNode, ...]

    @classmethod
    def uniform(cls, steps_per_mm, steps_per_deg, f_cpu=150_000_000,
                max_rate=80.0, accel=1000.0, profile=KNIFE):
        """
        Build an equal-XY (single belt/pulley) single-head machine using the
        conventional X=node1, Y=node2, Z=node3, A=node4 map. Convenience for the
        common simple case and for callers that only carry the three legacy
        scalars. The single head is centred (x_offset = 0) and carries `profile`
        (default KNIFE — the default machine is a tangential-knife machine).
        """
        head = ToolHead(
            z=AxisConfig(node=BusNode(3), steps_per_unit=steps_per_mm,  max_rate=max_rate, accel=accel),
            a=AxisConfig(node=BusNode(4), steps_per_unit=steps_per_deg, max_rate=max_rate, accel=accel,
                         rotary=True),
            profile=profile,
        )
        return cls(
            x=AxisConfig(node=BusNode(1), steps_per_unit=steps_per_mm, max_rate=max_rate, accel=accel),
            y=AxisConfig(node=BusNode(2), steps_per_unit=steps_per_mm, max_rate=max_rate, accel=accel),
            heads=(head,),
            f_cpu=f_cpu,
        )

    # ── active-head resolution — Z and A track the live head ──────────────────

    @property
    def head(self) -> "ToolHead":
        return self.heads[self.active_head]

    @property
    def z(self) -> AxisConfig:
        return self.heads[self.active_head].z

    @property
    def a(self) -> AxisConfig:
        return self.heads[self.active_head].a

    def present_axes(self):
        """
        [(letter, AxisConfig)] for axes whose bus node is fitted, in x,y,z,a
        order. Z and A resolve to the active head. Lets the host/GUI iterate the
        machine's real axes instead of assuming a fixed x/y/z/a set — drop a node
        (node.present = False) and it disappears from the UI.
        """
        return [(ltr, getattr(self, ltr)) for ltr in ("x", "y", "z", "a")
                if getattr(self, ltr).node.present]


def select_head(machine: MachineConfig, tool_name: str) -> int:
    """
    Index of the head carrying the named tool. The planner/pre-flight calls this
    per tool group — to run a knife job it picks the head whose profile is KNIFE.
    Raises ValueError if no mounted head has that tool (the operator must mount
    it). Does NOT mutate config; head selection is a runtime/plan concern, not a
    config edit.
    """
    for i, head in enumerate(machine.heads):
        if head.profile.name == tool_name:
            return i
    raise ValueError(f"no head has tool '{tool_name}' mounted")


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
    junction_deviation: float = 0.05   # mm — corner-rounding budget for the
                                # GRBL junction-deviation cornering cap (constrain
                                # stage). Algorithm tuning, so it lives here.
    ds_max:    float = 0.5      # mm — max spacing between flattened samples
                                # (redesign Flatten stage). Caps sample spacing so
                                # the look-ahead velocity passes have enough
                                # resolution for smooth accel/decel ramps even on
                                # long straight runs (premortem P3). Geometry
                                # (chord_tol) subdivides finer where curved.
    dtheta_max: float = 2.0     # deg — max tangent change between flattened
                                # samples (redesign Flatten stage). chord_tol
                                # bounds POSITION error but a tangential knife
                                # also needs bounded ANGULAR steps: on a curve,
                                # ds*kappa radians of tangent turn per sample. Cap
                                # it so the blade tracks smoothly instead of
                                # jogging in visible facets. Dominant cap on
                                # curves; ds_max/chord_tol dominate on straights.


def _default_machine() -> MachineConfig:
    """
    The physical machine: DM542 @ 1/32 microstepping.
      X/Y : GT2 20T pulley, 40 mm/rev -> 160 steps/mm
      Z   : lead screw -> 1200 steps/mm
      A   : tangential rotary -> 120 steps/deg
    Node map X=1, Y=2, Z=3, A=4. Single centred head (x_offset = 0), KNIFE
    mounted. No non-axis peripherals fitted yet.

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
    head = ToolHead(
        z=AxisConfig(node=BusNode(3), steps_per_unit=1200.0, max_rate=10.0, invert=True),  # PLACEHOLDER mm/s
        # a=AxisConfig(node=BusNode(4), steps_per_unit=51.667, rotary=True, max_rate=100.0,
        a=AxisConfig(node=BusNode(4), steps_per_unit=8.890, rotary=True, max_rate=100.0,
                     accel=2000.0, invert=True),  # PLACEHOLDER deg/s & deg/s^2;
        # invert confirmed by corner cut (vert edges flipped). a.accel bounds the
        # tangential A axis directly: it caps in-cut tracking acceleration (so the
        # TMC isn't commanded past its torque limit and drop steps) AND sets the
        # ramp for pure-A pivots. 2000 deg/s^2 leaves the current jobs unaffected
        # (the cap is slack at their curvature/feed) while protecting tighter/
        # faster cuts -- MEASURE the real TMC accel ceiling and replace.
        profile=KNIFE,
    )
    return MachineConfig(
        x=AxisConfig(node=BusNode(1), steps_per_unit=160.0, max_rate=80.0, accel=1000.0, invert=True),
        y=AxisConfig(node=BusNode(2), steps_per_unit=160.0, max_rate=80.0, accel=1000.0),
        heads=(head,),
    )


@dataclass(frozen=True)
class PipelineConfig:
    machine: MachineConfig = field(default_factory=_default_machine)
    quality: QualityConfig = field(default_factory=QualityConfig)


def default() -> PipelineConfig:
    """The single set of defaults. Every stage CLI sources its defaults here."""
    return PipelineConfig()


def load(path) -> PipelineConfig:
    """Load config from a TOML file. Deferred — not implemented yet."""
    raise NotImplementedError(
        "TOML config loading is not implemented yet; use config.default()"
    )
