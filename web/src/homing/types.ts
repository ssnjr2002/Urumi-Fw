/**
 * homing/types.ts — what a derived home looks like before anything moves.
 *
 * The point of naming these is that a home is fully decided before the first
 * step: four legs and a datum, all of it arithmetic on AxisConfig. Making that
 * plan a VALUE rather than a sequence of side effects is what lets it be
 * unit-tested, printed for an operator to check, and rejected — all without a
 * machine attached. Nothing here talks to a Link.
 */

import type { AxisLetter } from "../wire/format/status.js";

/** Which of the four legs this is. Ordering is the execution order. */
export const LegKind = {
    /** 1 — fast ramped approach. Ends ON the switch. */
    SEEK: "seek",
    /** 2 — short retract clear of the switch, at latch speed. */
    BACKOFF: "backoff",
    /** 3 — slow re-approach. Ends ON the switch; this leg sets repeatability. */
    LATCH: "latch",
    /** 4 — retract to the park point. Ends OFF the switch, machine IDLE. */
    PARK: "park",
    /**
     * Rotary — one evidence-terminated sweep through the index. Both legs of a
     * rotary home are this kind; only `dir` differs, and that is the point:
     * their answers straddle the truth and averaging cancels the difference.
     */
    SWEEP: "sweep",
} as const;
export type LegKind = (typeof LegKind)[keyof typeof LegKind];

/**
 * One leg command, in the units the wire takes.
 *
 * Step intervals rather than feeds because the conversion has happened: this is
 * the last representation before the command string, and keeping mm/s here
 * would mean two places could disagree about the arithmetic.
 */
export interface HomingLeg {
    readonly kind: LegKind;
    readonly axis: AxisLetter;
    /** The node's own direction sense — `invert` is already folded in. */
    readonly dir: 0 | 1;
    readonly startUs: number;
    readonly floorUs: number;
    readonly rampSteps: number;
    readonly maxSteps: number;
    /**
     * Whether the axis is expected to be ON its switch when this leg ends, i.e.
     * whether the Pico should finish in ALARM/LIMIT_LATCHED rather than IDLE.
     * True for SEEK and LATCH. Carried explicitly so the sequencer can CHECK the
     * terminal state instead of hardcoding which legs alarm.
     */
    readonly endsLatched: boolean;
    /** Human-readable, for a log or a dry run. Not parsed by anything. */
    readonly describe: string;
}

/**
 * A complete home for one axis: the legs, then the datum.
 *
 * `datumSteps` is the axis's machine position once leg 4 has finished — the
 * argument for the closing `setorigin`. It is NOT zero in general: leg 4 parks
 * a known distance clear of a switch whose own coordinate is known, so the
 * datum is that arithmetic. Homing a far-end switch on a 500 mm axis parked
 * 5 mm clear puts the axis at 495 mm, not at 0.
 */
export interface HomingPlan {
    readonly axis: AxisLetter;
    readonly legs: readonly HomingLeg[];
    readonly datumSteps: number;
}

/**
 * A rotary home: two sweeps, and what to do with the two answers.
 *
 * A separate type from HomingPlan rather than a variant of it, because the
 * datum is not a number that can be computed here. A linear plan knows
 * `datumSteps` before anything moves — leg 4 parks a known distance from a
 * switch whose coordinate is known. A rotary plan CANNOT: the index's position
 * is the measurement, so the datum is only available after both legs have run.
 * What is decidable in advance is what to do with the pair, which is these
 * fields.
 */
export interface RotaryHomingPlan {
    readonly axis: AxisLetter;
    /** Exactly two, identical but for `dir`. */
    readonly legs: readonly HomingLeg[];
    /** Steps per degree, carried so the sequencer need not re-read the config. */
    readonly stepsPerUnit: number;
    /** Nominal steps per revolution, `stepsPerUnit x 360`. The sweep MEASURES
     *  the real one; this is only what the budget was sized against. */
    readonly nominalStepsPerRev: number;
    /** Max allowed separation between the two answers, in steps. */
    readonly toleranceSteps: number;
    /** Machine coordinate the index itself is assigned, in steps. */
    readonly datumSteps: number;
}
