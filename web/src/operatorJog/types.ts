/**
 * operatorJog/types.ts — the interface a demo/production UI/CLI consumes.
 */

/** A handle on a running jog — the UI holds one for the duration of the move. */
export interface JogHandle {
    /** Resolves `true` once at rest, `false` on an abort or a failure. */
    done: Promise<boolean>;
    /** Stop the move (soft abort). */
    abort(): void;
}

/**
 * The per-axis calibration a jog helper needs. Most callers pass a
 * MachineConfig axis entry.
 */
export interface AxisCalibration {
    /** Steps per machine unit (mm, degrees, …). */
    readonly stepsPerUnit: number;
    /** The host's wiring inversion: wire steps = units × stepsPerUnit, negated. */
    readonly invert?: boolean;
    /** The Pico's: its machine units are wire steps over ±stepsPerUnit. */
    readonly invertDir?: boolean;
    /** The Pico's jog speed (units/s); a rate is sent as a multiple of it. */
    readonly jogFeed?: number;
}
