/**
 * schema.ts — the config types and their factories. Each factory fills absent
 * fields from DEFAULTS. Presets are in tools.ts, default values in
 * defaults.ts, resolution policy in resolve.ts.
 *
 *   bus     — BusNode: one RS485 node, by id, firmware type and whether fitted.
 *   machine — AxisConfig: a node with step maths and ceilings. MachineConfig:
 *             the X/Y gantry, the heads, peripherals, feeds and positions.
 *   head    — ToolHead: a Z + A pair, its offset, and the tools it accepts.
 *             One head is live at a time.
 *   tool    — ToolProfile: how a tool moves and cuts.
 *   quality — QualityConfig: the toolpath algorithms' tolerances.
 */

import { DEFAULTS } from "./defaults.js";

// ── bus tier ─────────────────────────────────────────────────────────────────

/** A node's firmware type, as include/common.h NODE_TYPE_*. */
export const NodeType = {
    STEPPER: 0x01,
    VACUUM: 0x02,
    KNIFE_OSC: 0x03,
} as const;

export type NodeType = (typeof NodeType)[keyof typeof NodeType];

/**
 * One node on the RS485 bus, always declared by what uses it: an axis or a
 * `peripherals[]` entry. There is no top-level node list.
 *
 * `present` is wiring, not liveness: false means designed in but not fitted.
 * Whether a present node answers is found on the bus, not here.
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
 * An operation's feed and accel in its own motion space, not a per-axis
 * ceiling. Absent feed falls to the machine's; absent accel to the axes'
 * ceilings.
 */
export interface OpTarget {
    readonly feed?: number;
    readonly accel?: number;
}

// ── machine tier (axes) ──────────────────────────────────────────────────────

/** Homing to a limit switch at one end of the travel. Feeds in mm/s. */
export interface LinearHoming {
    readonly kind: "linear";

    /** mm/s the seek ramps up to and cruises at. */
    readonly seekFeed: number;
    /** mm/s for the slow re-approach and the two retracts. Sets repeatability. */
    readonly latchFeed: number;
    /** Steps taken ramping startFeed → seekFeed. */
    readonly rampSteps: number;

    /** Passed to the blob unchecked; the controller's decoder checks them. */
    readonly cycle?: number;
    readonly seekPositive?: boolean;
    readonly seekScaler?: number;
    readonly startFeed?: number;
    readonly backoffDist?: number;
    readonly pullOffDist?: number;
    readonly parkPos?: number;
}

/**
 * Homing to a once-per-revolution Hall index. Not a superset of LinearHoming:
 * the index is found by running through it, so two sweeps are averaged, and a
 * rotary axis has no ends to bound or park clear of. Feeds in deg/s.
 */
export interface RotaryHoming {
    readonly kind: "rotary";

    /**
     * Runaway ceiling for a sweep, in revolutions. A sweep stops once its
     * crossings prove the period; this only bounds a broken axis. Below 3.2 a
     * healthy axis starting just past the index can report `notfound`.
     */
    readonly budgetRevs: number;

    /** deg/s the sweep ramps up to and cruises at. */
    readonly sweepFeed: number;
    /** Steps taken ramping startFeed → sweepFeed. */
    readonly rampSteps: number;

    /**
     * Largest disagreement between the two sweeps, degrees. Each sweep reads
     * the index a little late in its direction; averaging cancels that, and a
     * larger gap means slip, a second magnet or a wrong stepsPerUnit.
     */
    readonly toleranceDeg: number;

    /** Passed to the blob; see LinearHoming. */
    readonly cycle?: number;
    readonly startFeed?: number;
    readonly indexPos?: number;
}


/**
 * Tool-height probe for a Z axis: linear homing's four legs, with the switch
 * on another node at a fixed place on the bed.
 */
export interface ProbeConfig {
    /** The node the switch is on (the vacuum), shared by both heads' switches in series. */
    readonly node: BusNode;

    /**
     * Where the tool tip must be to press this head's switch, mm, tip frame.
     * The machine position differs per mounted tool, so it is converted at
     * move time.
     */
    readonly switchXMm: number;
    readonly switchYMm: number;

    /**
     * How long to wait for the vacuum's stream-byte reply, µs. Costs no
     * overtravel: in lockstep the controller emits nothing while it waits.
     */
    readonly replyDeadlineUs: number;

    /** How far below the start to search, mm; past it is PROBE_BUDGET. Cover the longest tool. */
    readonly probeTravel: number;

    /** mm/s the seek STARTS at, before the ramp. */
    readonly pullInFeed: number;
    /** mm/s the seek ramps up to. Fast; this leg does not measure. */
    readonly seekFeed: number;
    /** mm/s for the latch and both retracts. This number sets repeatability. */
    readonly latchFeed: number;
    /** Steps taken ramping pullInFeed -> seekFeed. */
    readonly rampSteps: number;

    /** Retract between seek and latch, mm. Must exceed the switch's release hysteresis. */
    readonly backoffMm: number;

    /** Final retract, mm -- where Z is left standing when the probe ends. */
    readonly parkMm: number;

    /**
     * How far Z may pass the switch on the seek leg, mm: the gap between polls,
     * `pollDiv = max(1, floor(seekOvertravelMm x stepsPerUnit))`. Larger seeks
     * faster and drives further into the bed. The latch polls every step.
     */
    readonly seekOvertravelMm: number;

    /**
     * Height of the switch's trip point above the mat, mm: the mat is the
     * contact height lowered by this. Positive when the switch trips above the
     * mat surface, negative when it sits below it. 0 = no correction.
     */
    readonly tripMm: number;
}

/** The two homing kinds, by `kind`; they share only their feeds. */
export type HomingConfig = LinearHoming | RotaryHoming;

export interface AxisConfig {
    readonly node: BusNode;
    readonly stepsPerUnit: number;
    /** Physical velocity ceiling (mm/s or deg/s). 0 = uncapped. */
    readonly maxFeed: number;
    /** Physical acceleration ceiling (mm/s² or deg/s²). 0 = uncapped. */
    readonly maxAccel: number;
    readonly maxTravel: number;
    readonly invertDir?: boolean;
    readonly softLimits?: boolean;
    /** Jog speeds (units/s), homed and unhomed. The loader fills both. */
    readonly jogFeed?: number;
    readonly jogFeedUnhomed?: number;
    readonly rotary: boolean;
    /**
     * Absent: no switch or index, the axis cannot be homed. Never defaulted:
     * every field is a measurement of this machine.
     */
    readonly homing?: HomingConfig;

    /** Absent: no bed switch, so Z is touched off by hand. A head's Z only. */
    readonly probe?: ProbeConfig;

    /**
     * A head's A only: the holder interlock. |machine A| within this many
     * degrees releases the tool; `mount` turns A past it to lock. Absent or 0,
     * or the A node absent: no interlock.
     */
    readonly unlockRange?: number;
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
 * An XY offset in mm from one reference point to another: a head from the
 * anchor, a tool tip from its head. Whatever sits at (0, 0) is the anchor.
 */
export interface ReferencePoint {
    readonly xOffset: number;
    readonly yOffset: number;
}

/**
 * Fixed XY offset of the tool tip from the head centre, mm, whatever A is.
 * Not `offsetMm`, the blade's trail, which turns with A.
 */
export type ToolOffset = ReferencePoint;

/**
 * Duty limits for a powered tool that cannot run continuously, in what the
 * controller can observe: a budget of on-time, and an off time that resets it.
 */
export interface DutyLimits {
    /** Hard budget of on-time between resets (seconds). */
    readonly maxOnS: number;
    /** Don't reset before this much on-time (seconds), so short subpaths don't each reset. */
    readonly minOnS: number;
    /** Off time that resets the tool (seconds). */
    readonly dwellS: number;
}

/** A powered tool, switched on before each plunge and off after each lift. */
export interface ToolPower {
    /**
     * The node that switches it: a present KNIFE_OSC peripheral, or the
     * laser's STEPPER node (accepted only by a controller built with
     * ACCEPT_LASER_AS_POWER_TOOL, for bench tests).
     */
    readonly node: { readonly id: number; readonly type: NodeType };
    /** Seconds from switching on to touching material, for the tool to come up to power. */
    readonly settleS: number;
    /** Absent = unlimited on-time. */
    readonly dutyLimits?: DutyLimits;
}

export interface ToolProfile {
    readonly name: string;
    /** The tool's id on the wire (1..255, unique). Absent only on the bare presets. */
    readonly id?: number;
    readonly toolType: ToolType;
    readonly tangential: boolean;
    /** Tangential: the blade tip's trail behind the A axis, mm. */
    readonly offsetMm: number;
    readonly unwind: boolean;
    /** A tangential tool looks the same every 360/periodCount degrees (knife 1, crease 2). */
    readonly periodCount: number;
    /** A corner turning a tangential tool more than this lifts to swivel (degrees). */
    readonly cornerAngleDeg: number;
    readonly minRadiusMm: number;
    /** Cable: |A| at most this either way from home (degrees); absent or 0 = no cable. */
    readonly aMaxDeg?: number;
    /** Tangential: the edge's heading at machine A = 0, the holder's lock (degrees); absent = 0. */
    readonly aOffsetDeg?: number;
    /** Cut feed, mm/s; absent → machine.feed.cutFallback. */
    readonly cutFeed?: number;
    /** Plunge feed, mm/s; absent → machine.feed.plungeFallback. */
    readonly plungeFeed?: number;
    /** Cuts into the material (cut height the mat), not on it (the material top). */
    readonly plunge: boolean;
    readonly requiredPeripheralTypes: readonly NodeType[];
    /** Fixed XY offset of tool tip from head center (mm). Default (0,0). */
    readonly toolOffset: ToolOffset;
    /** A revolver tool's chamber angles (degrees); A turns to chamberOffsets[i] for chamber i. */
    readonly chamberOffsets?: readonly number[];
    /** Absent on a passive tool. */
    readonly power?: ToolPower;
}

export function toolProfile(
    name: string,
    overrides?: Partial<Omit<ToolProfile, "name">>,
): ToolProfile {
    return { name, ...DEFAULTS.tool, ...overrides };
}

// ── head tier ────────────────────────────────────────────────────────────────

/**
 * One head: a Z + A pair and its XY offset from the anchor. A machine has one
 * or more, never run at once.
 *
 * `accepts` is what the head's fixture can hold, in order of preference; an
 * empty list is a head nothing mounts on. What is mounted now is live state,
 * not config.
 */
export interface ToolHead extends ReferencePoint {
    readonly z: AxisConfig;
    readonly a: AxisConfig;
    /** Tools whose fixture this socket takes, preference-ordered. See interface doc. */
    readonly accepts: readonly ToolType[];
    /** This head's tool-probe switch, a tip position in machine coordinates; each head reaches only its own. */
    readonly probeSwitch?: ProbeSwitch;
}

/** `accepts` defaults to empty, so an unconfigured head holds nothing. */
export function toolHead(
    z: AxisConfig,
    a: AxisConfig,
    overrides?: Partial<Omit<ToolHead, "z" | "a">>,
): ToolHead {
    return { z, a, xOffset: 0, yOffset: 0, accepts: [], ...overrides };
}

// ── machine tier (config) ────────────────────────────────────────────────────

/**
 * Optional laser pointer: when present it is the anchor, (0, 0), and heads are
 * offset from it. Switched through its node (`-DNODE_HAS_LASER`).
 */
export interface LaserPointer {
    /** Bus id of the node the laser is wired to. */
    readonly node: number;
}

/** A point on the bed in machine coordinates, mm. */
export interface BedPoint {
    readonly x: number;
    readonly y: number;
}

/** A head's probe switch; `z` is its height above the bed (mm), for `wzero z_switch`. */
export interface ProbeSwitch extends BedPoint {
    readonly z?: number;
}

/**
 * Anchor positions in machine coordinates. Absent `park`: where homing parks.
 * `mount`: where a job's tool change stops for the operator.
 */
export interface StoredPositions {
    readonly park?: BedPoint;
    readonly load?: BedPoint;
    readonly mount?: BedPoint;
}

/**
 * The rectangle a job's tips stay inside, bed mm (the laser dot on the point).
 * Absent: the axes' soft ranges alone. Jogs are not confined to it.
 */
export interface WorkArea {
    readonly xMin: number;
    readonly xMax: number;
    readonly yMin: number;
    readonly yMax: number;
}

export interface MachineConfig {
    readonly x: AxisConfig;
    readonly y: AxisConfig;
    readonly heads: readonly ToolHead[];
    /** The head mapped at boot, before anything picks one. */
    readonly defaultHead: number;
    readonly fCpu: number;
    readonly feed: JobFeed;
    /** Standalone-A slew (reposition; machine-owned). feed unset ⇒ A ceiling. */
    readonly slew: OpTarget;
    /** Lift and travel height above the material top, mm. */
    readonly zClearance: number;
    /** How far a zeroed tip may go below the bed (work Z 0), mm; the bed's underlay. */
    readonly maxOvercut: number;
    readonly peripherals: readonly BusNode[];
    /** Optional laser pointer module (alignment reference). */
    readonly laser?: LaserPointer;
    readonly positions: StoredPositions;
    /** Its min corner is also the work origin at boot and on `wclear`. */
    readonly workArea?: WorkArea;
    /** Relative jogs allowed before homing, without soft limits (testing). */
    readonly jogUnhomed: boolean;
    /** Bed mesh on at boot; `mesh on|off` changes it until reboot. */
    readonly meshOn: boolean;
    readonly buffer: JobBuffer;
}

/** Job feeds, mm/s, at least 1; the `feed` command overrides cut and travel. */
export interface JobFeed {
    /** Cut feed when neither the layer nor its tool sets one. */
    readonly cutFallback: number;
    /** Travel to a contour's start. */
    readonly travelDefault: number;
    /** Plunge feed when the tool sets none. */
    readonly plungeFallback: number;
    /** Lift feed, every tool. */
    readonly liftFeed: number;
}

/**
 * When a job leaves BUFFERING: once `queueTime` s of motion is queued, or once
 * no record has arrived for `idleTime` s. Jobs only; jogs start at once.
 */
export interface JobBuffer {
    readonly queueTime: number;
    readonly idleTime: number;
}

export function machineConfig(
    x: AxisConfig,
    y: AxisConfig,
    heads: readonly ToolHead[],
    overrides?: Partial<Omit<MachineConfig, "x" | "y" | "heads">>,
): MachineConfig {
    return {
        x, y, heads, ...DEFAULTS.machine,
        positions: {},
        jogUnhomed: false,
        meshOn: false,
        ...overrides,
    };
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
    /** Halvings allowed when a flatten step overshoots dsMax or dthetaMax. 0 disables the check. */
    readonly maxRefine: number;
}

export function qualityConfig(overrides?: Partial<QualityConfig>): QualityConfig {
    return { ...DEFAULTS.quality, ...overrides };
}

// ── controller config ────────────────────────────────────────────────────────

export interface ControllerConfig {
    readonly machine: MachineConfig;
    readonly quality: QualityConfig;
    readonly toolProfiles: Readonly<Record<string, ToolProfile>>;
}
