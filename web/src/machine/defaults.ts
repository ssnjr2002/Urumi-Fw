/**
 * defaults.ts — every default value that is not machine calibration
 * (stepsPerUnit and node ids have none and are required).
 *
 * Machine targets are filled at load, so MachineConfig is always complete.
 * Tool overrides stay undefined when absent, meaning "inherit from the
 * machine"; resolveTargets() in resolve.ts is the one place that chain is
 * spelled out.
 */

export const DEFAULTS = {
    machine: {
        fCpu: 150_000_000,
        /** Job feeds, mm/s: the cut and plunge fallbacks and the travel. */
        feed: { cutFallback: 80, travelDefault: 80, plungeFallback: 20, liftFeed: 20 },
        /** Standalone-A slew (reposition). Empty ⇒ falls to the A axis ceiling. */
        slew: {},
        /** Margin above the material top that counts as clear, mm. */
        zClearance: 2,
        maxOvercut: 0,
        /** Job buffering thresholds, s (follower.cpp). */
        buffer: { queueTime: 1, idleTime: 0.5 },
        defaultHead: 0,
        peripherals: [],
    },

    /** Axis fields that are not calibration. 0 = uncapped for the ceilings. */
    axis: {
        maxFeed: 0,
        maxAccel: 0,
        maxTravel: 0,
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
        periodCount: 1,
        cornerAngleDeg: 20,
        minRadiusMm: 0,
        plunge: false,
        requiredPeripheralTypes: [],
        toolOffset: { xOffset: 0, yOffset: 0 },
    },
} as const;
