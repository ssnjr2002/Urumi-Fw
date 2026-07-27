/**
 * dutyBreaks.ts — schedule enable-line resets for a duty-limited tool.
 *
 * See docs/tool_duty_limits.md. The short version: the ultrasonic knife shuts
 * itself off after ~40 s of continuous power and resets only when its enable
 * line is released for a second or two. Releasing it needs an RS485 relay,
 * which needs a free bus, which needs the machine PAUSED — so the reset has to
 * be a planned stop, baked into the segment stream.
 *
 * This stage is a PURE POST-PASS over already-baked segments. It emits no
 * motion and moves nothing; it only ORs marker flags onto segments that already
 * exist. That is possible because every lift already sits at zero velocity —
 * `plan` forces v=0 at PATH_END and `constrain` forces it at corners — so a
 * lift is a place the machine can already stop cleanly. A baked MICRO_PAUSE
 * does NOT decelerate (core1.cpp), and honouring that is the whole reason this
 * stage restricts itself to lifts.
 *
 * The consequence, and the current limitation: a stretch of toolpath with NO
 * lift in it cannot be broken, because breaking it would mean inserting a stop
 * where the planner did not plan one. That needs a constrain→plan→discretize
 * iteration and is not implemented — `scheduleDutyBreaks` throws rather than
 * let a cut silently overrun its budget.
 *
 * BUDGET ACCOUNTING. A break is an interval, not an instant, and most of that
 * interval is POWERED — so the break spends the budget it exists to protect.
 * Of the interval's parts, only the dwell is free (the tool is off; that is the
 * reset). The lift, plunge and the decel into the stop are all real segments
 * and are counted by segmentSeconds automatically. `settleS` is the one part
 * with no segment behind it — the runner waits it after re-asserting, before
 * moving — so it is charged explicitly to the head of each window after a
 * break. Inserting a lift adds decel/lift/plunge/accel that likewise are not
 * in the stream when the choice is made; when tier 2 lands, those get a
 * WORST-CASE reserve (feedMax/accel for the ramps, zSteps/zFeed for the Z)
 * subtracted from both ends of the band, so the schedule is safe on the first
 * pass rather than relying on an iteration to converge.
 */

import type { ResolvedAxes } from "../config/config.js";
import type { DutyLimits } from "../config/schema.js";
import {
    MICRO_LIFT,
    MICRO_PAUSE,
    MICRO_DUTY_RELEASE,
    MICRO_DUTY_ASSERT,
    type MicroSegment,
} from "../wire/format/microsegment.js";

/** Wall-clock seconds a segment occupies: its major axis steps × interval. */
export function segmentSeconds(s: MicroSegment, fCpu: number): number {
    const major = Math.max(Math.abs(s.dx), Math.abs(s.dy), Math.abs(s.dz), Math.abs(s.da));
    return (major * s.interval) / fCpu;
}

/**
 * A lift-to-plunge opportunity found in the baked stream.
 *
 * `raiseIdx` is the Z-up segment, `lowerIdx` the next Z-down, and `offWindowS`
 * the time between them — the pivot on a corner, or the travel plus
 * re-orientation at a subpath end. A candidate QUALIFIES when that window
 * already covers dwellS, in which case a reset there costs nothing.
 */
interface Candidate {
    readonly raiseIdx: number;
    readonly lowerIdx: number;
    /** Cumulative seconds at the moment the raise completes. */
    readonly atS: number;
    readonly offWindowS: number;
}

/**
 * Find every raise→lower pair, with the elapsed time at each and the width of
 * the gap between them.
 *
 * Z invert is applied inside choreograph.zMove, so the sign of `dz` in the
 * baked stream is in WIRE space — un-apply it before asking "is this a raise?",
 * or every candidate is inverted on a machine with an inverted Z.
 */
function findCandidates(
    segments: readonly MicroSegment[],
    axes: ResolvedAxes,
    fCpu: number,
): Candidate[] {
    const trueDz = (s: MicroSegment): number => (axes.z.invert ? -s.dz : s.dz);

    const out: Candidate[] = [];
    let t = 0;
    let openRaise: { idx: number; at: number } | null = null;
    let gap = 0;

    for (let i = 0; i < segments.length; i++) {
        const s = segments[i]!;
        const dur = segmentSeconds(s, fCpu);
        const isLift = (s.flags & MICRO_LIFT) !== 0;
        const dz = trueDz(s);

        if (isLift && dz > 0) {
            // A raise. Any previously open raise had no matching lower before
            // this one (shouldn't happen, but don't carry it forward).
            t += dur;
            openRaise = { idx: i, at: t };
            gap = 0;
            continue;
        }

        if (isLift && dz < 0 && openRaise) {
            out.push({
                raiseIdx: openRaise.idx,
                lowerIdx: i,
                atS: openRaise.at,
                offWindowS: gap,
            });
            openRaise = null;
            gap = 0;
            t += dur;
            continue;
        }

        t += dur;
        if (openRaise) gap += dur;
    }
    return out;
}

export interface DutyBreakResult {
    /** A fresh array; the input is never mutated. */
    readonly segments: MicroSegment[];
    /** Cumulative seconds at each scheduled reset, for reporting. */
    readonly breaksAtS: readonly number[];
}

/**
 * Mark reset points in a baked segment stream.
 *
 * Banded greedy (docs §5): within [minOnS, maxOnS] of the last reset, take the
 * LATEST candidate — preferring one whose off-window already covers dwellS,
 * since that one is free. Taking the latest maximises cutting between resets,
 * which is what minimises their number.
 *
 * Both markers land on the raise segment: the runner releases, waits out any
 * shortfall, and re-asserts before the machine moves again. Splitting them
 * across the gap (release at the raise, assert after the travel) is the
 * documented follow-up and needs no change here beyond where the flags go.
 *
 * Throws when the band holds no candidate at all — see the file header.
 */
export function scheduleDutyBreaks(
    segments: readonly MicroSegment[],
    duty: DutyLimits,
    axes: ResolvedAxes,
    fCpu: number,
    toolName = "tool",
): DutyBreakResult {
    const candidates = findCandidates(segments, axes, fCpu);
    const flags = new Map<number, number>();
    const breaksAtS: number[] = [];

    const totalS = segments.reduce((acc, s) => acc + segmentSeconds(s, fCpu), 0);

    let lastResetS = 0;
    // Settle time is POWERED time: the runner re-asserts the enable line, waits
    // settleS for the tool to come up, and only then moves. It burns budget
    // without appearing in any segment, so every window after a break is
    // settleS shorter than its segment durations suggest. The first window is
    // not charged — nothing has been re-asserted yet.
    let settleCharge = 0;

    for (;;) {
        // Budget consumed by a candidate at time `c.atS`, counting the powered
        // settle at the head of this window.
        const spent = (atS: number): number => atS - lastResetS + settleCharge;

        if (spent(totalS) <= duty.maxOnS) break; // the tail fits

        // `c.atS > lastResetS` is not implied by the band: a settle wide enough
        // relative to minOnS pushes the band's lower edge behind the last reset,
        // and picking a candidate there would walk lastResetS BACKWARDS and
        // loop forever. Resets are monotonic in time, always.
        const inBand = candidates.filter(
            (c) =>
                c.atS > lastResetS &&
                spent(c.atS) > duty.minOnS &&
                spent(c.atS) <= duty.maxOnS,
        );
        if (inBand.length === 0) {
            const lo = (lastResetS + duty.minOnS - settleCharge).toFixed(1);
            const t = (lastResetS + duty.maxOnS - settleCharge).toFixed(1);
            throw new Error(
                `'${toolName}': no lift between ${lo}s and ${t}s ` +
                    `to release the enable line at, and inserting one is not implemented ` +
                    `(docs/tool_duty_limits.md §10). Shorten the path, raise dutyLimits.maxOnS, ` +
                    `or lower minOnS to widen the band.`,
            );
        }

        // Prefer a free candidate (off-window already covers the dwell); among
        // equals take the latest. Falling back to the latest of any kind costs
        // an idle dwell but still avoids cutting into the geometry.
        const free = inBand.filter((c) => c.offWindowS >= duty.dwellS);
        const pick = (free.length ? free : inBand).reduce((a, b) => (b.atS > a.atS ? b : a));

        flags.set(
            pick.raiseIdx,
            (flags.get(pick.raiseIdx) ?? 0) | MICRO_PAUSE | MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT,
        );
        breaksAtS.push(pick.atS);
        lastResetS = pick.atS;
        settleCharge = duty.settleS;
    }

    const out = segments.map((s, i) => {
        const add = flags.get(i);
        return add === undefined ? s : { ...s, flags: s.flags | add };
    });
    return { segments: out, breaksAtS };
}
