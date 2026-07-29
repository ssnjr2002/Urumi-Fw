/**
 * defaults.ts — every default value that is NOT machine calibration.
 *
 * The single home for policy numbers. Before this file the cut-feed default
 * (80) existed in four places: machineConfig(), the loader's fallback, and
 * trailing `?? 80` guards in compileBlock/discretize/walk. Changing it meant
 * grepping. Now: change it here, once.
 *
 * What belongs here: operation targets, axis field defaults, quality tuning,
 * fCpu. What does NOT: stepsPerUnit, invert, node ids — those are per-machine
 * calibration and must come from config.json (see load.ts's header on why
 * there is no silent fallback for them).
 *
 * Interaction with optional config.json fields — fill-at-load, one exception:
 *
 *   machine.*  targets are FILLED at load. An absent `machine.path` becomes
 *              DEFAULTS.machine.path, so MachineConfig is always fully
 *              populated and consumers never write `??`.
 *
 *   tool.*     overrides are LEFT UNDEFINED when absent, because `undefined`
 *              there means "inherit from machine" — real information that
 *              filling would destroy.
 *
 * The chain (tool → machine → default) is spelled out in exactly one place:
 * resolveTargets() in helpers.ts. Stages call that, never `??`.
 */

export const DEFAULTS = {
    machine: {
        fCpu: 150_000_000,
        /** Cut target baseline (engage; tools override). */
        path: { feed: 80 },
        /** Pen-up XY travel (reposition; machine-owned). */
        rapid: { feed: 80 },
        /** Z engage baseline (tools override). */
        z: { feed: 20 },
        /** Standalone-A slew (reposition). Empty ⇒ falls to the A axis ceiling. */
        slew: {},
        defaultHead: 0,
        peripherals: [],
    },

    /** Axis fields that are not calibration. 0 = uncapped for the ceilings. */
    axis: {
        maxFeed: 0,
        maxAccel: 0,
        maxTravel: 0,
        invert: false,
        rotary: false,
    },

    quality: {
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
        maxRefine: 8,
    },

    tool: {
        toolType: 0x01 as const, // ToolType.PEN — literal to avoid a config.ts cycle
        tangential: false,
        offsetMm: 0,
        unwind: false,
        cornerAngleDeg: 20,
        minRadiusMm: 0,
        liftHeight: 0,
        requiredPeripheralTypes: [],
        toolOffset: { xOffset: 0, yOffset: 0 },
    },
} as const;
