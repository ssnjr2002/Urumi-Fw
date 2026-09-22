/**
 * operatorJog/types.ts — the interface a demo/production UI/CLI consumes.
 */

/** A handle on a running jog — the UI holds one for the duration of the move. */
export interface JogHandle {
    /** Resolves `true` on success/truncation, `false` on fatal. */
    done: Promise<boolean>;
    /** End the session at the next frame boundary. Reversal or stop. */
    abort(): void;
}

/** Tuning knobs for the open-session ClickJogSource, all optional. */
export interface ClickJogSourceOptions {
    /** Rest velocity in steps/s (default 50, the minimum the machine maintains). */
    vStart?: number;
    /** Keep at most this much motion-time queued ahead in microseconds (default 250 ms). */
    leadUs?: number;
    /** Segment count low-water mark for pacing (default 16). */
    lowWater?: number;
    /** Motion time per emitted packet in ms (default 20). */
    chunkMs?: number;
    /** Maximum packets per pull() call (default 16). */
    maxBurst?: number;
}

export interface JogToOptions {
    /** Rest velocity in steps/s (default 50). */
    vStart?: number;
}

/**
 * The per-axis calibration a jog helper needs. Most callers extract this from
 * a MachineConfig axis entry.
 */
export interface AxisCalibration {
    /** Steps per machine unit (mm, degrees, …). */
    readonly stepsPerUnit: number;
    /**
     * Wiring inversion: if true, sign the planner chose is flipped before
     * the jog packet is built. The planner defines the coordinate frame;
     * every source of motion must apply the same inversion or the machine
     * has two disagreeing frames.
     */
    readonly invert?: boolean;
}