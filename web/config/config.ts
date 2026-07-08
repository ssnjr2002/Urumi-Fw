/**
 * config.ts — single source of truth for all pipeline parameters.
 *
 * The stages stay pure functions that receive these values explicitly; this
 * module is only where the values are DEFINED.
 *
 * Five tiers:
 *
 *   bus     — BusNode: the RS485 topology primitive. A node can be anything
 *             (stepper axis, knife controller, suction valve). node_id is the
 *             RS485 address; role names what the board drives; present=false
 *             marks a node declared but not fitted.
 *   machine — AxisConfig: a BusNode WITH step-math (steps/mm or steps/deg,
 *             max_rate, accel, invert, rotary). MachineConfig: the per-machine
 *             definition — X/Y shared gantry, one or more ToolHeads, bus
 *             peripherals, travel defaults (jog_feed, z_feed), and f_cpu.
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

// ── bus tier ─────────────────────────────────────────────────────────────────

// TODO: Think about BusNode.present and required config. We dont say busnode
// in a json config, we just have axis and peripherals but there is no way to
// state if they are present or not? I dont have clarity on this. Btw present
// means its wired up on the bus, not that its alive or something. Maybe rethink
// the name?

// TODO: simplify `nodeId` to just `id` — the field is on `BusNode` already,
// so `node.nodeId` is redundant; `node.id` reads cleaner. Deferred to avoid
// a wide rename across the codebase; the configLoader maps JSON `nodeId`
// straight through for now.
export interface BusNode {
    readonly nodeId: number;
    readonly role: string;
    readonly present: boolean;
}

export function busNode(
    nodeId: number,
    overrides?: Partial<Omit<BusNode, "nodeId">>,
): BusNode {
    return { nodeId, role: "stepper", present: true, ...overrides };
}

// ── machine tier (axes) ──────────────────────────────────────────────────────

export interface AxisConfig {
    readonly node: BusNode;
    readonly stepsPerUnit: number;
    readonly maxRate: number;
    readonly accel: number;
    readonly maxTravel: number;
    readonly invert: boolean;
    readonly rotary: boolean;
}

export function axisConfig(
    node: BusNode,
    stepsPerUnit: number,
    overrides?: Partial<Omit<AxisConfig, "node" | "stepsPerUnit">>,
): AxisConfig {
    return {
        node,
        stepsPerUnit,
        maxRate: 0,
        accel: 0,
        maxTravel: 0,
        invert: false,
        rotary: false,
        ...overrides,
    };
}

// ── tool tier ────────────────────────────────────────────────────────────────

export const OFFSET_TOLERANCE_MM = 0.05;

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

export interface ToolProfile {
    readonly name: string;
    readonly toolType: ToolType;
    readonly tangential: boolean;
    readonly offsetMm: number;
    readonly unwind: boolean;
    readonly cornerAngleDeg: number;
    readonly minRadiusMm: number;
    readonly feedMax: number;
    readonly accel: number;
    readonly liftHeight: number;
    readonly zFeed: number;
    readonly jogFeed: number;
    readonly requiredPeripheralRoles: readonly string[];
    /** Fixed XY offset of tool tip from head center (mm). Default (0,0). */
    readonly toolOffset: ToolOffset;
    /**
     * A-axis slot offset angles (degrees) for a revolver tool. Absent on
     * PEN/KNIFE/CREASE. Present on REVOLVER_PEN — the orchestrator jogs A
     * to slotOffsets[i] before cutting with slot i.
     */
    readonly slotOffsets?: readonly number[];
}

export function toolProfile(
    name: string,
    overrides?: Partial<Omit<ToolProfile, "name">>,
): ToolProfile {
    return {
        name,
        toolType: ToolType.PEN,
        tangential: false,
        offsetMm: 0,
        unwind: false,
        cornerAngleDeg: 20,
        minRadiusMm: 0,
        feedMax: 80,
        accel: 0,
        liftHeight: 0,
        zFeed: 0,
        jogFeed: 0,
        requiredPeripheralRoles: [],
        toolOffset: { xOffset: 0, yOffset: 0 },
        ...overrides,
    };
}

export const PEN: ToolProfile = toolProfile("pen", {
    toolType: ToolType.PEN,
    tangential: false,
});

export const KNIFE: ToolProfile = toolProfile("knife", {
    toolType: ToolType.KNIFE,
    tangential: true,
    offsetMm: 0,
    unwind: true,
    cornerAngleDeg: 20,
});

export const CREASE: ToolProfile = toolProfile("crease", {
    toolType: ToolType.CREASE,
    tangential: true,
    offsetMm: 0,
    unwind: false,
    cornerAngleDeg: 30,
});

/**
 * Revolver pen: a rotating module with 7 slots for pens. The A axis
 * selects which slot is active (lowered). Each slot has a defined A
 * offset angle (360/7 ≈ 51.43° intervals). The active pen tip is at a
 * fixed XY offset from the head center regardless of which slot is
 * active — set via toolOffset.
 *
 * Not tangential — the A axis is used for slot selection, not tangent
 * tracking. The orchestrator jogs A to slotOffsets[i] before cutting
 * with slot i.
 *
 * slotOffsets: 7 angles at 360/7 intervals, starting at 0°.
 * toolOffset: placeholder (0, -R) — replace with the real pen tip
 * offset from the head center once measured.
 */
const REVOLVER_SLOT_COUNT = 7;
const REVOLVER_SLOT_INTERVAL = 360 / REVOLVER_SLOT_COUNT;
export const REVOLVER_PEN: ToolProfile = toolProfile("revolver_pen", {
    toolType: ToolType.REVOLVER_PEN,
    tangential: false,
    cornerAngleDeg: 30,
    slotOffsets: Array.from(
        { length: REVOLVER_SLOT_COUNT },
        (_, i) => i * REVOLVER_SLOT_INTERVAL,
    ),
    // TODO: measure the real pen tip offset from head center
    toolOffset: { xOffset: 0, yOffset: 0 },
});

export const TOOL_PROFILES: Readonly<Record<string, ToolProfile>> = {
    pen: PEN,
    knife: KNIFE,
    crease: CREASE,
    revolver_pen: REVOLVER_PEN,
};

export const TOOL_PROFILES_BY_TYPE: Readonly<Record<number, ToolProfile>> = {
    [ToolType.PEN]: PEN,
    [ToolType.KNIFE]: KNIFE,
    [ToolType.CREASE]: CREASE,
    [ToolType.REVOLVER_PEN]: REVOLVER_PEN,
};

/**
 * True if the tool's blade offset is large enough to require (unimplemented)
 * offset compensation. The discretize stage refuses to run with a tool that
 * needs offset comp — it would cut wrong silently. Raise OFFSET_TOLERANCE_MM
 * only once compensation exists.
 */
export function needsOffsetComp(profile: ToolProfile): boolean {
    return profile.offsetMm > OFFSET_TOLERANCE_MM;
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
    return { z, a, profile: PEN, xOffset: 0, yOffset: 0, ...overrides };
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
    readonly jogFeed: number;
    readonly zFeed: number;
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
    return {
        x,
        y,
        heads,
        defaultHead: 0,
        fCpu: 150_000_000,
        jogFeed: 80,
        zFeed: 20,
        peripherals: [],
        ...overrides,
    };
}

/**
 * Build an equal-XY (single belt/pulley) single-head machine using the
 * conventional X=node1, Y=node2, Z=node3, A=node4 map. Convenience for the
 * common simple case. The single head is centred (x_offset = 0) and carries
 * `profile` (default KNIFE).
 */
export function uniformMachine(
    stepsPerMm: number,
    stepsPerDeg: number,
    options?: {
        fCpu?: number;
        maxRate?: number;
        accel?: number;
        profile?: ToolProfile;
    },
): MachineConfig {
    const { fCpu = 150_000_000, maxRate = 80, accel = 1000, profile = KNIFE } = options ?? {};
    const head = toolHead(
        axisConfig(busNode(3), stepsPerMm, { maxRate, accel }),
        axisConfig(busNode(4), stepsPerDeg, { maxRate, accel, rotary: true }),
        { profile },
    );
    return machineConfig(
        axisConfig(busNode(1), stepsPerMm, { maxRate, accel }),
        axisConfig(busNode(2), stepsPerMm, { maxRate, accel }),
        [head],
        { fCpu },
    );
}

/**
 * Resolved axes: the 4 AxisConfig (x, y, z, a) + fCpu as a flat slice.
 * Z and A resolve to the default head. Used by wire/choreograph/discretize
 * which need the 4 axes but don't want to re-resolve the head on every call.
 */
export interface ResolvedAxes {
    readonly x: AxisConfig;
    readonly y: AxisConfig;
    readonly z: AxisConfig;
    readonly a: AxisConfig;
    readonly fCpu: number;
}

/** Resolve the 4 axes from a MachineConfig (Z/A from the default head). */
export function resolvedAxes(machine: MachineConfig): ResolvedAxes {
    const head = machine.heads[machine.defaultHead]!;
    return { x: machine.x, y: machine.y, z: head.z, a: head.a, fCpu: machine.fCpu };
}

/**
 * The physical machine: DM542 @ 1/32 microstepping.
 *   X/Y : GT2 20T pulley, 40 mm/rev -> 160 steps/mm
 *   Z   : lead screw -> 1200 steps/mm
 *   A   : tangential rotary -> 51.667 steps/deg
 * Node map X=1, Y=2, Z=3, A=4. Single centred head (x_offset = 0), KNIFE
 * mounted. No non-axis peripherals fitted.
 */
function defaultMachine(): MachineConfig {
    const head = toolHead(
        axisConfig(busNode(3), 1200.0, { maxRate: 10.0, invert: true }),
        axisConfig(busNode(4), 51.667, {
            maxRate: 100.0,
            accel: 2000.0,
            invert: true,
            rotary: true,
        }),
        { profile: KNIFE },
    );
    return machineConfig(
        axisConfig(busNode(1), 160.0, { maxRate: 80.0, accel: 1000.0, invert: true }),
        axisConfig(busNode(2), 160.0, { maxRate: 80.0, accel: 1000.0 }),
        [head],
    );
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
}

export function qualityConfig(overrides?: Partial<QualityConfig>): QualityConfig {
    return {
        chordTol: 0.01,
        dvMax: 3.0,
        vMin: 0.5,
        dtMax: 0.05,
        dtMin: 1e-6,
        angleTol: 5.0,
        gapTol: 0.01,
        nKappa: 20,
        junctionDeviation: 0.05,
        dsMax: 0.5,
        dthetaMax: 2.0,
        ...overrides,
    };
}

// ── pipeline config ──────────────────────────────────────────────────────────

export interface PipelineConfig {
    readonly machine: MachineConfig;
    readonly quality: QualityConfig;
    readonly toolProfiles: Readonly<Record<string, ToolProfile>>;
}

export function pipelineConfig(overrides?: Partial<PipelineConfig>): PipelineConfig {
    return {
        machine: defaultMachine(),
        quality: qualityConfig(),
        toolProfiles: { ...TOOL_PROFILES },
        ...overrides,
    };
}

/** The single set of defaults. Every stage sources its defaults here. */
export function defaultConfig(): PipelineConfig {
    return pipelineConfig();
}
