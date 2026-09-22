/**
 * operatorJog/jogTo.ts — closed-session absolute go-to-coordinate helpers.
 *
 * Reads the current position from the machine's STATUS_RSP, computes the
 * wire-frame delta, builds a trapezoidal jog via makeJog, and streams it as a
 * closed session. Returns a JogHandle — the UI awaits .done and may call
 * .abort() mid-move (session.truncate + Link.abort, §4.5 soft abort).
 *
 * Two entry points, same machinery: `jogTo` moves ONE axis, `jogToPoint` moves
 * any combination of the four in a single coordinated move. They are not
 * different mechanisms — a single-axis go-to is just a point with three axes
 * left unspecified — so `jogTo` is a wrapper and there is one implementation to
 * reason about.
 *
 * Only one jog may be active on a Link at a time. The UI enforces this.
 */

import { Link } from "../wire/link/link.js";
import { ListSource, Session } from "../wire/link/session.js";
import { makeJog } from "./makeJog.js";
import type { AxisCalibration, JogHandle, JogToOptions } from "./types.js";

const F_CPU = 150_000_000;
const V_START_DEFAULT = 50;

/**
 * An absolute destination. Each entry is an axis index (0=x 1=y 2=z 3=a) mapped
 * to its target in machine units and the calibration to convert it with. An
 * axis absent from the map is not commanded — it holds position, which is
 * different from targeting its current value (that would still be planned, and
 * rounding could move it a step).
 */
export type JogTarget = {
    readonly axisIndex: number;
    readonly axis: AxisCalibration;
    readonly targetPos: number;
};

/**
 * Absolute coordinated go-to across up to four axes.
 *
 * All commanded axes start and stop together: makeJog runs Bresenham against
 * whichever axis has the most steps, so the move is a straight line in step
 * space rather than a sequence of per-axis moves. This matters for anything
 * where the tool is down — moving X then Y traces an L, not a diagonal.
 *
 * `rate` is the feed of the MAJOR axis in its own units per second; minor axes
 * are slower in proportion to their share of the move, which is what keeps the
 * path straight. Axes already at their target contribute a zero delta and drop
 * out; if every axis does, the move is a no-op and resolves true.
 */
export function jogToPoint(
    link: Link,
    targets: readonly JogTarget[],
    rate: number,
    opts?: JogToOptions,
): JogHandle {
    let sess: Session | null = null;
    const run = (async (): Promise<boolean> => {
        const st = await link.getStatus();

        const vec: [number, number, number, number] = [0, 0, 0, 0];
        for (const t of targets) {
            const inv = t.axis.invert ? -1 : 1;
            const wireTarget = Math.round(t.targetPos * t.axis.stepsPerUnit * inv);
            vec[t.axisIndex] = wireTarget - st.pos![t.axisIndex]!;
        }

        // The major axis sets the feed. Picking it by step count (not by the
        // caller's argument order) is what makes the rate mean the same thing
        // however the targets were listed.
        let major = targets[0];
        let majorSteps = -1;
        for (const t of targets) {
            const s = Math.abs(vec[t.axisIndex]!);
            if (s > majorSteps) { majorSteps = s; major = t; }
        }
        if (!major || majorSteps <= 0) return true;   // already there

        const spu = major.axis.stepsPerUnit;
        const feedSps = Math.max(1, rate * spu);
        const accelSps2 = Math.max(rate * 8, 50) * spu;
        const packets = makeJog(vec, feedSps, accelSps2, F_CPU, opts?.vStart ?? V_START_DEFAULT);

        sess = link.session(new ListSource(packets));
        return await sess.run();
    })();

    return {
        done: run,
        abort: () => {
            sess?.truncate();
            link.abort();
        },
    };
}

/** Absolute go-to on a single axis — `jogToPoint` with one target. */
export function jogTo(
    link: Link,
    axis: AxisCalibration,
    axisIndex: number, // 0=x 1=y 2=z 3=a
    targetPosMm: number,
    rateMmPerSec: number,
    opts?: JogToOptions,
): JogHandle {
    return jogToPoint(link, [{ axisIndex, axis, targetPos: targetPosMm }], rateMmPerSec, opts);
}
