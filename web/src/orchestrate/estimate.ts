/**
 * orchestrate/estimate.ts — how long a walk will take on the wire.
 *
 * Lives beside the walk rather than in machine/ because it is a property of
 * compiled events, not of the machine: the only thing it needs from the machine
 * is fCpu, which it takes as a number.
 */

import type { WalkEvent } from "./walk.js";
import type { MicroSegment } from "../wire/format/microsegment.js";

/**
 * Total motion time in seconds at `fCpu`.
 *
 * Mirrors core1.cpp's emitMicroSegment: a segment runs `major` steps waiting
 * `interval` cycles between each, so its cost is interval x major. Summing
 * `interval` alone — the obvious-looking version, and the one that gets written
 * first — treats every segment as a single step and under-reports by orders of
 * magnitude on a long path.
 *
 * Pauses contribute nothing: they last as long as the operator takes.
 */
export function walkSeconds(events: readonly WalkEvent[], fCpu: number): number {
    let cycles = 0;
    for (const e of events) {
        if (e.kind !== "motion") continue;
        for (const s of e.segments) cycles += s.interval * majorSteps(s);
    }
    return cycles / fCpu;
}

/** Steps on the dominant axis — the one the interval is timed against. */
export function majorSteps(s: MicroSegment): number {
    return Math.max(Math.abs(s.dx), Math.abs(s.dy), Math.abs(s.dz), Math.abs(s.da));
}

/** Every motion segment in a walk, in order. */
export function motionSegments(events: readonly WalkEvent[]): readonly MicroSegment[] {
    return events.flatMap((e) => (e.kind === "motion" ? e.segments : []));
}
