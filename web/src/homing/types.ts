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
} as const;
export type LegKind = (typeof LegKind)[keyof typeof LegKind];

/**
 * One `home` command, in the units the wire takes.
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
