/**
 * homing/derive.ts — AxisConfig + LinearHoming → the four legs and the datum.
 *
 * PURE. No Link, no clock, no I/O. That is deliberate and it is the main reason
 * this file exists separately from sequence.ts: every number that could send an
 * axis into a hard stop is computed here, and here can be tested exhaustively
 * with nothing plugged in. sequence.ts is then only "send, poll, check".
 *
 * The arithmetic, in one place (docs/homing.md §3.4):
 *
 *   interval_us  = 1e6 / (feed × stepsPerUnit)
 *   approachDir  = (!atOrigin XOR invert) ? 1 : 0
 *   seek budget  = hardTravel × stepsPerUnit × SEEK_MARGIN
 *   datum        = (atOrigin ? parkMm : hardTravel − parkMm) × stepsPerUnit
 */

import type { AxisConfig, LinearHoming, RotaryHoming } from "../machine/schema.js";
import type { AxisLetter } from "../wire/format/status.js";
import { LegKind, type HomingLeg, type HomingPlan, type RotaryHomingPlan } from "./types.js";

/**
 * Overshoot allowance on the seek budget. The budget is a RUNAWAY cap, not a
 * distance: a seek stops itself at the switch, so the only thing this number
 * decides is how far past the whole frame the axis may travel before the Pico
 * concludes the switch will never arrive. Under 1.0 would abort a legitimate
 * home started from the far end.
 */
const SEEK_MARGIN = 1.1;

/**
 * Slack on leg 3's budget. Leg 3 re-approaches from backoffMm away, so it needs
 * a little over backoffMm — but it is a SEEK and stops at the switch, so being
 * generous costs nothing except the length of a fault timeout on a genuinely
 * broken axis. Being stingy reports "switch never found" on a healthy one.
 */
const LATCH_MARGIN = 2.5;

/** Step interval for a feed, µs, floored at 1 — the wire field is a uint16. */
function intervalUs(feed: number, stepsPerUnit: number): number {
    const us = Math.round(1e6 / (feed * stepsPerUnit));
    return Math.max(1, Math.min(65535, us));
}

/**
 * The node-side direction bit that moves this axis TOWARD its switch.
 *
 * Two independent facts compose here and both must be right. `atOrigin` is
 * geometry — which end of the frame the switch sits at. `invert` is wiring —
 * whether the node's dir=1 increases or decreases the axis coordinate. Either
 * one alone tells you nothing; only the XOR gives an approach direction.
 *
 * Exported because a bring-up console driving single legs by hand needs the
 * same answer, and re-deriving it there is exactly how the two would drift.
 */
export function approachDir(axis: AxisConfig, h: LinearHoming): 0 | 1 {
    return (!h.atOrigin) !== axis.invert ? 1 : 0;
}

/** Steps for a distance in axis units, at least 1 — a zero-step leg cannot run. */
function steps(mm: number, stepsPerUnit: number): number {
    return Math.max(1, Math.round(mm * stepsPerUnit));
}

/**
 * Build the full four-leg plan for one axis.
 *
 * Throws if the axis has no `homing` block. Not a soft failure: the caller
 * asked to home an axis that has no switch, and returning an empty plan would
 * let a "home all" quietly skip an axis and then report success.
 */
export function derivePlan(
    axisLetter: AxisLetter,
    axis: AxisConfig,
    homing: LinearHoming | undefined = asLinear(axis.homing),
): HomingPlan {
    if (homing === undefined) {
        throw new Error(`axis ${axisLetter} has no linear homing config — it has no limit switch`);
    }
    const spu = axis.stepsPerUnit;
    const toward = approachDir(axis, homing);
    const away: 0 | 1 = toward === 1 ? 0 : 1;

    const pullInUs = intervalUs(homing.pullInFeed, spu);
    const seekUs = intervalUs(homing.seekFeed, spu);
    const latchUs = intervalUs(homing.latchFeed, spu);

    const backoffSteps = steps(homing.backoffMm, spu);
    const parkSteps = steps(homing.parkMm, spu);

    const leg = (
        kind: typeof LegKind[keyof typeof LegKind],
        dir: 0 | 1,
        startUs: number,
        floorUs: number,
        rampSteps: number,
        maxSteps: number,
        endsLatched: boolean,
        describe: string,
    ): HomingLeg => ({
        kind, axis: axisLetter, dir, startUs, floorUs, rampSteps,
        maxSteps: Math.round(maxSteps), endsLatched, describe,
    });

    const legs: readonly HomingLeg[] = [
        leg(LegKind.SEEK, toward, pullInUs, seekUs, homing.rampSteps,
            homing.hardTravel * spu * SEEK_MARGIN, true,
            `seek ${homing.seekFeed} mm/s over up to ${homing.hardTravel} mm`),

        // The retracts run at latchFeed, not seekFeed. They are short, so the
        // time costs nothing, and leg 2 in particular is leaving a switch whose
        // release point is what leg 3 will measure against.
        leg(LegKind.BACKOFF, away, latchUs, latchUs, 0, backoffSteps, false,
            `back off ${homing.backoffMm} mm clear of the switch`),

        // Budgeted from backoffMm, not hardTravel: leg 3 starts backoffMm away
        // and the switch is the only thing it can hit. A hardTravel budget here
        // would let a failed leg 2 (still on the switch, so leg 3 arms as a
        // RETRACT) drive the full frame in the wrong direction.
        leg(LegKind.LATCH, toward, latchUs, latchUs, 0,
            backoffSteps * LATCH_MARGIN, true,
            `re-approach at ${homing.latchFeed} mm/s`),

        leg(LegKind.PARK, away, latchUs, latchUs, 0, parkSteps, false,
            `park ${homing.parkMm} mm clear`),
    ];

    // Where leg 4 leaves the axis, in machine coordinates. A switch at the 0 end
    // means parking parkMm ABOVE zero; a far-end switch means parking parkMm
    // BELOW hardTravel. Getting atOrigin backwards is invisible here and
    // catastrophic downstream, which is why it has no default in the schema.
    const datumMm = homing.atOrigin ? homing.parkMm : homing.hardTravel - homing.parkMm;

    return { axis: axisLetter, legs, datumSteps: Math.round(datumMm * spu) };
}

// ── rotary ──────────────────────────────────────────────────────────

/** Narrow a HomingConfig to the linear arm, or undefined. */
function asLinear(h: AxisConfig["homing"]): LinearHoming | undefined {
    return h !== undefined && h.kind === "linear" ? h : undefined;
}

/** Narrow a HomingConfig to the rotary arm, or undefined. */
function asRotary(h: AxisConfig["homing"]): RotaryHoming | undefined {
    return h !== undefined && h.kind === "rotary" ? h : undefined;
}

/**
 * Build the two-sweep plan for a rotary axis.
 *
 * The two legs are IDENTICAL but for `dir`, and that symmetry is the method
 * rather than a convenience. Each sweep measures the index a little late in
 * whichever direction it is travelling, so the two answers straddle the truth by
 * equal amounts and their midpoint is the truth — a cancellation that does not
 * require the cause to be diagnosed correctly. Giving the two legs different
 * feeds would break it, since the error would then differ between them.
 *
 * Which direction runs FIRST does not matter, unlike linear homing, where a
 * retract before a seek is meaningless. A rotary axis has no ends and no state
 * to be in: either sweep is a legal opening move. dir=1 is first only so the
 * order is fixed for anyone reading a log.
 *
 * `maxSteps` is a runaway ceiling and NOT a distance — see RotaryHoming
 * .budgetRevs. It is sized against the NOMINAL steps per revolution
 * (`stepsPerUnit x 360`), which is the configured gearing rather than the
 * measured one, because on the very first sweep of a new head no measurement
 * exists yet. The two differ by well under a percent in practice (the bench
 * measures 16554 against a configured 16498), which a 4x ceiling absorbs
 * without noticing.
 */
export function deriveRotaryPlan(
    axisLetter: AxisLetter,
    axis: AxisConfig,
    homing: RotaryHoming | undefined = asRotary(axis.homing),
): RotaryHomingPlan {
    if (homing === undefined) {
        throw new Error(`axis ${axisLetter} has no rotary homing config — it has no index`);
    }
    const spu = axis.stepsPerUnit;
    const nominalStepsPerRev = spu * 360;
    const maxSteps = Math.round(nominalStepsPerRev * homing.budgetRevs);

    const pullInUs = intervalUs(homing.pullInFeed, spu);
    const sweepUs = intervalUs(homing.sweepFeed, spu);

    const sweep = (dir: 0 | 1): HomingLeg => ({
        kind: LegKind.SWEEP,
        axis: axisLetter,
        dir,
        startUs: pullInUs,
        floorUs: sweepUs,
        rampSteps: homing.rampSteps,
        maxSteps,
        // A rotary node has no limit pin, so its LIMIT bit is permanently 0 and
        // there is no latch for a sweep to end in. False is the truth here, not
        // a default.
        endsLatched: false,
        describe:
            `sweep ${dir === 1 ? "forward" : "reverse"} at ${homing.sweepFeed} deg/s, ` +
            `up to ${homing.budgetRevs} rev`,
    });

    return {
        axis: axisLetter,
        legs: [sweep(1), sweep(0)],
        stepsPerUnit: spu,
        nominalStepsPerRev,
        toleranceSteps: Math.max(1, Math.round(homing.toleranceDeg * spu)),
        datumSteps: Math.round(homing.datumDeg * spu),
    };
}

/**
 * Fold a difference into the half-open interval (-period/2, +period/2].
 *
 * Exported for the tests, not for callers: the folding is the step that can
 * silently produce a wrong datum, so it is worth pinning against real bench
 * numbers rather than reasoning about.
 *
 * Folding to the SIGNED half range rather than [0, period) is load-bearing. A
 * small NEGATIVE bias would otherwise come back as period-minus-a-bit, i.e. as
 * an enormous disagreement, and fail a tolerance check on a healthy machine.
 */
export function foldSigned(delta: number, period: number): number {
    const m = ((delta % period) + period) % period;
    return m > period / 2 ? m - period : m;
}

/** The two sweeps' answers, reduced to one index and the evidence for it. */
export interface ResolvedIndex {
    /** Mean of the two measured steps-per-revolution. */
    readonly stepsPerRev: number;
    /** How far apart those two measurements were, in steps. */
    readonly revSpread: number;
    /**
     * Half the folded separation between the two indexes, signed — the one-way
     * measurement bias that averaging removes.
     */
    readonly biasSteps: number;
    /** The averaged index, in the node's own counter frame. */
    readonly indexSteps: number;
}

/**
 * Combine a forward and a reverse sweep into the index that lies between them.
 *
 * PURE, and the reason the two-leg sequence exists. Each sweep reports the index
 * slightly late in whichever direction it was travelling, so the two answers
 * straddle the truth by equal amounts and the midpoint is the truth. The
 * cancellation does not depend on the cause being diagnosed correctly — anything
 * that is odd in direction dies at the midpoint — which is why this is preferred
 * over correcting one sweep by a stored offset.
 *
 * The two indexes are in the SAME frame: the node's counter runs continuously
 * across both legs, so they are directly comparable once the whole revolutions
 * between them are folded away. On the bench that was 48947 and 15789 against a
 * period of ~16554 — two laps plus 49 steps, where the 49 is the entire signal
 * and the two laps are an artefact of where the sweeps happened to stop.
 *
 * Judging the results is the caller's: this reports `revSpread` and `biasSteps`
 * and has no opinion about whether either is too large, because the thresholds
 * are config and the failure is an operator-facing message.
 */
export function resolveRotaryIndex(
    forwardIndex: number,
    forwardStepsPerRev: number,
    reverseIndex: number,
    reverseStepsPerRev: number,
): ResolvedIndex {
    const stepsPerRev = (forwardStepsPerRev + reverseStepsPerRev) / 2;
    const biasSteps = foldSigned(forwardIndex - reverseIndex, stepsPerRev) / 2;
    return {
        stepsPerRev,
        revSpread: Math.abs(forwardStepsPerRev - reverseStepsPerRev),
        biasSteps,
        indexSteps: reverseIndex + biasSteps,
    };
}
