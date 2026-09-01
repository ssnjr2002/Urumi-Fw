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

import type { AxisConfig, LinearHoming } from "../machine/schema.js";
import type { AxisLetter } from "../wire/format/status.js";
import { LegKind, type HomingLeg, type HomingPlan } from "./types.js";

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
    homing: LinearHoming | undefined = axis.homing,
): HomingPlan {
    if (homing === undefined) {
        throw new Error(`axis ${axisLetter} has no homing config — it has no limit switch`);
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
