/**
 * operatorJog/jogTo.ts — absolute go-to-coordinate helpers over `jog`.
 *
 * Reads the current position from the machine's STATUS_RSP, turns each axis's
 * wire-frame delta into a `jog <axis> <dist> [scale]` (the Pico plans the
 * move), and resolves once the machine is back at rest. Returns a JogHandle —
 * the UI awaits .done and may call .abort() mid-move (Link.abort, §4.5 soft
 * abort).
 *
 * `jogTo` moves ONE axis, `jogToPoint` several; a single-axis go-to is a point
 * with three axes left unspecified, so `jogTo` is a wrapper.
 */

import { Link } from "../wire/link/link.js";
import { MachineState } from "../wire/format/status.js";
import { jog, type JogAxis } from "../wire/link/commands.js";
import type { AxisCalibration, JogHandle } from "./types.js";

const AXES: readonly JogAxis[] = ["x", "y", "z", "a"];
const POLL_MS = 50;
/** How long a `jog` may take to show as motion in STATUS_RSP. */
const START_MS = 500;

/**
 * An absolute destination. Each entry is an axis index (0=x 1=y 2=z 3=a) mapped
 * to its target in machine units and the calibration to convert it with. An
 * axis absent from the map is not commanded — it holds position, which is
 * different from targeting its current value (rounding could move it a step).
 */
export type JogTarget = {
    readonly axisIndex: number;
    readonly axis: AxisCalibration;
    readonly targetPos: number;
};

/**
 * Absolute go-to across up to four axes, one `jog` per axis that moves. The
 * Pico runs a line on one axis set at a time, so X and Y go one after the
 * other (joining without a stop where the corner allows), then Z and A.
 *
 * `rate` is each axis's feed in its own units per second, sent as a multiple of
 * that axis's jogFeed; the Pico caps it at maxFeed. Axes already at their
 * target drop out; if every axis does, the move is a no-op and resolves true.
 */
export function jogToPoint(
    link: Link,
    targets: readonly JogTarget[],
    rate: number,
): JogHandle {
    let aborted = false;
    const run = (async (): Promise<boolean> => {
        const st = await link.getStatus();
        let moved = false;
        for (const t of targets) {
            const inv = t.axis.invert ? -1 : 1;
            const wireTarget = Math.round(t.targetPos * t.axis.stepsPerUnit * inv);
            const steps = wireTarget - st.pos![t.axisIndex]!;
            if (steps === 0) continue;
            // The Pico's machine units: wire steps over its signed steps/unit.
            const dist = steps / (t.axis.stepsPerUnit * (t.axis.invertDir ? -1 : 1));
            const scale = t.axis.jogFeed ? rate / t.axis.jogFeed : 1;
            await jog(link, AXES[t.axisIndex]!, dist, scale);
            moved = true;
        }
        return moved ? await atRest(link, () => aborted) : true;
    })();

    return {
        done: run,
        abort: () => {
            aborted = true;
            link.abort();
        },
    };
}

/** Absolute go-to on a single axis — `jogToPoint` with one target. */
export function jogTo(
    link: Link,
    axis: AxisCalibration,
    axisIndex: number, // 0=x 1=y 2=z 3=a
    targetPos: number,
    rate: number,
): JogHandle {
    return jogToPoint(link, [{ axisIndex, axis, targetPos }], rate);
}

/**
 * True once the queued jogs have run out (IDLE); false on an abort or on any
 * state but JOGGING and IDLE.
 */
async function atRest(link: Link, aborted: () => boolean): Promise<boolean> {
    const t0 = Date.now();
    let seen = false;
    for (;;) {
        const s = (await link.getStatus()).state;
        if (aborted()) return false;
        if (s === MachineState.JOGGING) seen = true;
        else if (s === MachineState.IDLE) {
            if (seen || Date.now() - t0 > START_MS) return true;
        } else return false;
        await new Promise(r => setTimeout(r, POLL_MS));
    }
}
