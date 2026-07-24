/**
 * operatorJog/jogClick.ts — open-session blend helper for tap/click jogging.
 *
 * One click = one fixed distance (the button's mm value). Clicking again while
 * the machine is still moving extends the live move rather than starting a
 * fresh one — the blend. The UI holds the returned JogHandle, awaits `.done`,
 * and calls `.abort()` on reversal/stop.
 *
 * Only one jog may be active on a Link at a time — two sessions sharing the
 * same ack sink would cross-correlate. The UI enforces this (mirroring
 * Python's `OnlineSession.busy`), NOT this library.
 */

import type { Link } from "../wire/link/link.js";
import { ClickJogSource } from "./clickJogSource.js";
import type { AxisCalibration, ClickJogSourceOptions, JogHandle } from "./types.js";

/**
 * Start a tap/click jog on one axis. `sign` is +1 or -1 (coordinate-frame
 * direction); `distMm` is the distance per click (always positive). The
 * helper converts coordinate-frame distance to the steps the machine needs.
 *
 * The session is OPEN — re-clicking the same axis+sign extends the move;
 * clicking the opposite axis or sign calls `.abort()` on the prior handle
 * (which fires a soft abort via Link.abort, §4.5) and starts a new source.
 */
export function jogClick(
    link: Link,
    axis: AxisCalibration,
    axisLetter: "x" | "y" | "z" | "a",
    sign: number,
    distMm: number,
    rateMmPerSec: number,
    opts?: ClickJogSourceOptions,
): JogHandle {
    const steps = Math.max(1, Math.round(Math.abs(distMm) * axis.stepsPerUnit));
    const src = new ClickJogSource(axis, axisLetter, sign, rateMmPerSec, link, opts);
    src.add(steps);

    const done = link.session(src).run();
    return {
        done,
        abort: () => src.cancel(),
    };
}