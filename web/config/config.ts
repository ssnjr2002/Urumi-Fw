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
} as const;

export type ToolType = (typeof ToolType)[keyof typeof ToolType];

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

export const TOOL_PROFILES: Readonly<Record<string, ToolProfile>> = {
    pen: PEN,
    knife: KNIFE,
    crease: CREASE,
};

export const TOOL_PROFILES_BY_TYPE: Readonly<Record<number, ToolProfile>> = {
    [ToolType.PEN]: PEN,
    [ToolType.KNIFE]: KNIFE,
    [ToolType.CREASE]: CREASE,
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

export interface ToolHead {
    readonly z: AxisConfig;
    readonly a: AxisConfig;
    readonly profile: ToolProfile;
    readonly xOffset: number;
}

export function toolHead(
    z: AxisConfig,
    a: AxisConfig,
    overrides?: Partial<Omit<ToolHead, "z" | "a">>,
): ToolHead {
    return { z, a, profile: PEN, xOffset: 0, ...overrides };
}

// ── machine tier (config) ────────────────────────────────────────────────────

export interface MachineConfig {
    readonly x: AxisConfig;
    readonly y: AxisConfig;
    readonly heads: readonly ToolHead[];
    readonly defaultHead: number;
    readonly fCpu: number;
    readonly jogFeed: number;
    readonly zFeed: number;
    readonly peripherals: readonly BusNode[];
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
