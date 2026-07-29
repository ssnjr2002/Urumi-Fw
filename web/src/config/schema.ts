/**
 * schema.ts — the config TYPES and their factories. Nothing else.
 *
 * No tool presets (tools.ts), no hardcoded machines (fixtures.ts), no default
 * VALUES (defaults.ts), no resolution policy (resolve.ts). Every factory here
 * fills absent fields from DEFAULTS and is otherwise pure shape.
 *
 * The stages stay pure functions that receive these values explicitly; this
 * module is only where the SHAPES are defined.
 *
 * Five tiers:
 *
 *   bus     — BusNode: the RS485 topology primitive. A node can be anything
 *             (stepper axis, knife controller, suction valve). id is the RS485
 *             address; type names the node's firmware identity; present=false
 *             marks a node declared but not fitted.
 *   machine — AxisConfig: a BusNode WITH step-math (steps/mm or steps/deg,
 *             maxFeed/maxAccel ceilings, invert, rotary). MachineConfig: the
 *             per-machine definition — X/Y shared gantry, one or more ToolHeads,
 *             bus peripherals, operation targets (path/rapid/z/slew), and fCpu.
 *   head    — ToolHead: a co-mounted Z + A pair plus the tool mounted on it
 *             and its X mounting offset. A machine has one or more heads;
 *             only one is live at a time. The default head is declared
 *             statically via defaultHead — runtime head selection is NOT a
 *             config concern.
 *   tool    — ToolProfile: per-tool kinematic behaviour (tangential tracking,
 *             lift, corner handling, cut feed). Presets keyed by ToolType
 *             (PEN / KNIFE / CREASE). A new tool is a new preset, never a code
 *             change.
 *   quality — QualityConfig: algorithm tuning (chord_tol, ds_max, dtheta_max,
 *             junction_deviation, velocity/accel discretization caps).
 */

import { DEFAULTS } from "./defaults.js";

// ── bus tier ─────────────────────────────────────────────────────────────────

/**
 * Node type — the RS485 node's firmware identity, a numeric mirror of the
 * include/common.h NODE_TYPE_* enum. CMD_GET_TYPE returns this byte; the
 * orchestrator validates each node's reported type against config at connect
 * time (see docs/node_type_architecture.md §2). Same stable-byte-value pattern
 * as ToolType.
 */
export const NodeType = {
    STEPPER: 0x01,
    VACUUM: 0x02,
    KNIFE_OSC: 0x03,
} as const;

export type NodeType = (typeof NodeType)[keyof typeof NodeType];

/**
 * One node on the RS485 bus.
 *
 * A BusNode is never declared on its own — it is always declared BY the thing
 * that uses it: an axis (`machine.x.node`, `heads[i].z.node`, …) or a
 * `peripherals[]` entry. There is deliberately no top-level node list, because
 * a node with no consumer is not something the pipeline can act on: nothing
 * would address it. So the config states what the machine HAS, and the bus
 * topology falls out of that.
 *
 * `present` is about WIRING, not liveness: false means "this node is accounted
 * for in the design but not physically fitted". Nothing here is ever inferred
 * from a ping — connect-time reachability and node-type agreement are the
 * orchestrator's business (docs/node_type_architecture.md §2). A config with
 * present: true and an unplugged node is a valid config describing a broken
 * machine, and it is the orchestrator that must say so.
 */
export interface BusNode {
    readonly id: number;
    readonly type: NodeType;
    readonly present: boolean;
}

export function busNode(
    id: number,
    overrides?: Partial<Omit<BusNode, "id">>,
): BusNode {
    return { id, type: NodeType.STEPPER, present: true, ...overrides };
}

// ── operation target (feed/accel) ─────────────────────────────────────────────

/**
 * An operation's desired feed / accel — a scalar in the operation's own motion
 * space, NOT a per-axis ceiling. Both optional: an omitted value falls through
 * to the machine baseline (feed) or the participating-axis ceiling (accel).
 * See docs/feed_accel_value_model.md.
 */
export interface OpTarget {
    readonly feed?: number;
    readonly accel?: number;
}

/**
 * An operation target at the MACHINE tier, where fill-at-load guarantees a
 * feed (see defaults.ts). Distinct from OpTarget — a tool-tier target keeps
 * `feed` optional because `undefined` there means "inherit", which is real
 * information. This is the type-level statement of that difference, and it is
 * what lets consumers drop their `?? 80` fallbacks without a `!`.
 *
 * `accel` stays optional at both tiers: unset means "derive from the axis
 * ceilings", which no single number expresses.
 */
export interface MachineTarget {
    readonly feed: number;
    readonly accel?: number;
}

// ── machine tier (axes) ──────────────────────────────────────────────────────

export interface AxisConfig {
    readonly node: BusNode;
    readonly stepsPerUnit: number;
    /** Physical velocity ceiling (mm/s or deg/s). 0 = uncapped. */
    readonly maxFeed: number;
    /** Physical acceleration ceiling (mm/s² or deg/s²). 0 = uncapped. */
    readonly maxAccel: number;
    readonly maxTravel: number;
    readonly invert: boolean;
    readonly rotary: boolean;
}

export function axisConfig(
    node: BusNode,
    stepsPerUnit: number,
    overrides?: Partial<Omit<AxisConfig, "node" | "stepsPerUnit">>,
): AxisConfig {
    return { node, stepsPerUnit, ...DEFAULTS.axis, ...overrides };
}

// ── tool tier ────────────────────────────────────────────────────────────────

export const ToolType = {
    PEN: 0x01,
    KNIFE: 0x02,
    CREASE: 0x03,
    REVOLVER_PEN: 0x04,
} as const;

export type ToolType = (typeof ToolType)[keyof typeof ToolType];

/**
 * A 2D offset in mm from one reference point to another. Used for:
 *   - head offsets (head center vs machine reference)
 *   - laser pointer position (vs machine reference)
 *   - tool tip offset (tool tip vs head center)
 *
 * Convention: whichever party has (0, 0) defines the machine reference.
 * In a typical dual-head + laser setup, laser is at (0, 0), head 1 at
 * (-50, 0), head 2 at (+50, 0). In a single-head setup, the head is at
 * (0, 0), no laser.
 */
export interface ReferencePoint {
    readonly xOffset: number;
    readonly yOffset: number;
}

/**
 * Fixed XY offset of the tool tip from the head center, in mm.
 *
 * Distinct from `offsetMm` (the knife blade caster offset, which is
 * along the direction of travel and rotates with A). The toolOffset is
 * a constant XY shift — e.g. the revolver pen's active pen tip is at a
 * fixed offset from the head center regardless of which slot is active
 * or what the A angle is.
 *
 * Applied as a bake-time geometry shift: all of a tool's path
 * coordinates are shifted by `-toolOffset` before baking, so the baked
 * paths are in "head center" coordinates. The orchestrator then only
 * needs to account for `headOffset` when positioning the head.
 */
export type ToolOffset = ReferencePoint;

/**
 * Duty limits for a tool that cannot run continuously (docs/tool_duty_limits.md).
 *
 * The ultrasonic knife's controller shuts itself off after ~40 s of continuous
 * power and resets only when its enable line is released for a second or two.
 * Rather than model heat, the planner works in the only terms it can observe:
 * a budget of run time, and an off duration it must find room for.
 *
 * Grouped rather than spread flat across ToolProfile because the fields are
 * interdependent — a budget with no dwell is a config error, and validate can
 * only say so if it sees them together. Absent means no limit, so pens and
 * crease tools carry none of this.
 */
export interface DutyLimits {
    /** Hard budget of enable-line-on time between resets (seconds). */
    readonly maxOnS: number;
    /**
     * Don't reset before this much has elapsed (seconds). Without it a drawing
     * of many short subpaths would reset at every one of them for no benefit.
     */
    readonly minOnS: number;
    /** Required release duration for the tool to reset (seconds). */
    readonly dwellS: number;
    /**
     * Lead time between re-asserting and touching material (seconds) — an
     * ultrasonic transducer needs a moment to reach full amplitude, and a blade
     * that enters material below amplitude wedges rather than cuts.
     */
    readonly settleS: number;
}

export interface ToolProfile {
    readonly name: string;
    readonly toolType: ToolType;
    readonly tangential: boolean;
    readonly offsetMm: number;
    readonly unwind: boolean;
    readonly cornerAngleDeg: number;
    readonly minRadiusMm: number;
    /** Cut (pen-down) target; overrides machine.path. feed omitted → machine default. */
    readonly path?: OpTarget;
    /** Engage (touch-down / retract) target; overrides machine.z. */
    readonly z?: OpTarget;
    readonly liftHeight: number;
    readonly requiredPeripheralTypes: readonly NodeType[];
    /** Fixed XY offset of tool tip from head center (mm). Default (0,0). */
    readonly toolOffset: ToolOffset;
    /**
     * A-axis slot offset angles (degrees) for a revolver tool. Absent on
     * PEN/KNIFE/CREASE. Present on REVOLVER_PEN — the orchestrator jogs A
     * to slotOffsets[i] before cutting with slot i.
     */
    readonly slotOffsets?: readonly number[];
    /**
     * Duty limit for a tool that cannot run continuously. Absent = unlimited,
     * which is every tool except the ultrasonic knife.
     */
    readonly dutyLimits?: DutyLimits;
}

export function toolProfile(
    name: string,
    overrides?: Partial<Omit<ToolProfile, "name">>,
): ToolProfile {
    return { name, ...DEFAULTS.tool, ...overrides };
}

// ── head tier ────────────────────────────────────────────────────────────────

/**
 * One physical tool head: a co-mounted Z + A pair, the tool mounted on
 * it, and its XY mounting offset from the machine reference.
 *
 * A machine has one or more heads. On the current machine there is a
 * single centred head (offset 0, 0). A dual-head machine fixes two
 * heads side by side; they are software-selected, NEVER run
 * simultaneously, so only one head's Z/A are "live" at a time.
 *
 * A head is a *socket*: fixed geometry (Z + A wiring, XY mounting offset).
 * `xOffset`/`yOffset` is the head's position relative to the machine
 * reference (see ReferencePoint). When a non-centred head is active, every
 * XY move must be corrected by this offset — applied by the orchestrator as
 * a head-switch jog, not consumed by the bake pipeline.
 *
 * `profile?` is a SEED mount only — which tool the socket boots with. It is
 * NOT authoritative at runtime: an operator can swap tools without editing
 * config, so the orchestrator tracks the live head→tool assignment in a
 * mutable mount table (seeded from this field). Bake never reads it — bake
 * feasibility is a node-presence check (see canRunTool), not a mount check.
 * Absent = an empty socket at boot.
 */
export interface ToolHead extends ReferencePoint {
    readonly z: AxisConfig;
    readonly a: AxisConfig;
    /** Seed mount (boot-time tool); absent = empty socket. See interface doc. */
    readonly profile?: ToolProfile;
}

export function toolHead(
    z: AxisConfig,
    a: AxisConfig,
    overrides?: Partial<Omit<ToolHead, "z" | "a">>,
): ToolHead {
    return { z, a, xOffset: 0, yOffset: 0, ...overrides };
}

// ── machine tier (config) ────────────────────────────────────────────────────

/**
 * Optional laser pointer module. When present, defines the machine
 * reference point (the laser is at (0, 0) by convention). Heads are
 * positioned relative to the laser. When absent, the head at (0, 0) is
 * the reference.
 *
 * The laser is a passive alignment aid — it doesn't move, doesn't have
 * axes, and isn't on the bus. It's purely a geometric reference for
 * head offset calculations.
 */
export type LaserPointer = ReferencePoint;

export interface MachineConfig {
    readonly x: AxisConfig;
    readonly y: AxisConfig;
    readonly heads: readonly ToolHead[];
    readonly defaultHead: number;
    readonly fCpu: number;
    /** Cut target baseline (engage; tools override). */
    readonly path: MachineTarget;
    /** Pen-up XY travel target (reposition; machine-owned). */
    readonly rapid: MachineTarget;
    /** Z engage target baseline (tools override). */
    readonly z: MachineTarget;
    /** Standalone-A slew (reposition; machine-owned). feed unset ⇒ A ceiling. */
    readonly slew: OpTarget;
    readonly peripherals: readonly BusNode[];
    /** Optional laser pointer module (alignment reference). */
    readonly laser?: LaserPointer;
}

export function machineConfig(
    x: AxisConfig,
    y: AxisConfig,
    heads: readonly ToolHead[],
    overrides?: Partial<Omit<MachineConfig, "x" | "y" | "heads">>,
): MachineConfig {
    return { x, y, heads, ...DEFAULTS.machine, ...overrides };
}

// ── quality tier ─────────────────────────────────────────────────────────────

export interface QualityConfig {
    readonly chordTol: number;
    readonly dvMax: number;
    readonly vMin: number;
    readonly dtMax: number;
    readonly dtMin: number;
    readonly angleTol: number;
    readonly gapTol: number;
    readonly nKappa: number;
    readonly junctionDeviation: number;
    readonly dsMax: number;
    readonly dthetaMax: number;
    /**
     * Halvings allowed when a flatten step overshoots dsMax or dthetaMax
     * (audit F1/F7). Bounds how many samples a cusp can cost. 0 disables
     * enforcement, restoring the pre-F7 predict-and-hope behaviour.
     */
    readonly maxRefine: number;
}

export function qualityConfig(overrides?: Partial<QualityConfig>): QualityConfig {
    return { ...DEFAULTS.quality, ...overrides };
}

// ── pipeline config ──────────────────────────────────────────────────────────

export interface PipelineConfig {
    readonly machine: MachineConfig;
    readonly quality: QualityConfig;
    readonly toolProfiles: Readonly<Record<string, ToolProfile>>;
}
