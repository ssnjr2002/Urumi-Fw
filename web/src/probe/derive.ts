/**
 * probe/derive.ts — a Z axis's ProbeConfig → the four probe legs. PURE.
 *
 *   seek     down, ramped, polls every `seekOvertravelMm`   ends on CONTACT
 *   backoff  up `backoffMm` at latchFeed                    ends CLEAR
 *   latch    down at latchFeed, polls every step            ends on CONTACT  ← measures
 *   park     up `parkMm` at latchFeed                       ends CLEAR
 *
 * "Down" is toward the bed, which is +Z (heights.ts zDownSign).
 */

import type { AxisConfig, ProbeConfig } from "../machine/schema.js";
import type { ProbeLegArgs } from "../wire/link/commands.js";

/** Slack on the latch budget: it re-approaches from `backoffMm` away. */
const LATCH_MARGIN = 2.5;

export interface ProbeStep {
    readonly name: "seek" | "backoff" | "latch" | "park";
    readonly args: ProbeLegArgs;
}

export interface ProbePlan {
    /** Bus id of the node the switch is on. */
    readonly switchNode: number;
    readonly probe: ProbeConfig;
    readonly legs: readonly ProbeStep[];
}

function intervalUs(feed: number, spu: number): number {
    return Math.max(1, Math.min(65535, Math.round(1e6 / (feed * spu))));
}

function steps(mm: number, spu: number): number {
    return Math.max(1, Math.round(mm * spu));
}

/** Throws if `z` has no probe block. */
export function deriveProbePlan(z: AxisConfig): ProbePlan {
    const probe = z.probe;
    if (probe === undefined) throw new Error("this Z axis has no probe config");
    const spu = z.stepsPerUnit;
    const down: 0 | 1 = z.invert ? 0 : 1;
    const up: 0 | 1 = down === 1 ? 0 : 1;

    const latchUs = intervalUs(probe.latchFeed, spu);
    const backoff = steps(probe.backoffMm, spu);
    const seekPoll = Math.max(1, Math.floor(probe.seekOvertravelMm * spu));
    if (seekPoll > 255) {
        throw new RangeError(
            `seekOvertravelMm ${probe.seekOvertravelMm} is ${seekPoll} steps; the poll ` +
                "divisor is at most 255",
        );
    }

    const slow = (
        name: ProbeStep["name"], dir: 0 | 1, maxSteps: number, retract: boolean,
    ): ProbeStep => ({
        name,
        args: {
            dir, startUs: latchUs, ceilUs: latchUs, rampSteps: 0, pollDiv: 1,
            maxSteps: Math.round(maxSteps), deadlineUs: probe.replyDeadlineUs, retract,
        },
    });

    return {
        switchNode: probe.node.id,
        probe,
        legs: [
            {
                name: "seek",
                args: {
                    dir: down,
                    startUs: intervalUs(probe.pullInFeed, spu),
                    ceilUs: intervalUs(probe.seekFeed, spu),
                    rampSteps: probe.rampSteps,
                    pollDiv: seekPoll,
                    maxSteps: steps(probe.probeTravel, spu),
                    deadlineUs: probe.replyDeadlineUs,
                    retract: false,
                },
            },
            slow("backoff", up, backoff, true),
            slow("latch", down, backoff * LATCH_MARGIN, false),
            slow("park", up, steps(probe.parkMm, spu), true),
        ],
    };
}
