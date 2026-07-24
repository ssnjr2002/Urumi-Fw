/**
 * operatorJog/jogTo.ts — closed-session absolute go-to-coordinate helper.
 *
 * Reads the current axis position from the machine's STATUS_RSP, computes the
 * wire-frame delta, builds a trapezoidal jog via makeJog, and streams it as a
 * closed session. Returns a JogHandle — the UI awaits .done and may call
 * .abort() mid-move (session.truncate + Link.abort, §4.5 soft abort).
 *
 * Only one jog may be active on a Link at a time. The UI enforces this.
 */

import { Link } from "../wire/link/link.js";
import { ListSource, Session } from "../wire/link/session.js";
import { makeJog } from "./makeJog.js";
import type { AxisCalibration, JogHandle, JogToOptions } from "./types.js";

const F_CPU = 150_000_000;
const V_START_DEFAULT = 50;

export function jogTo(
    link: Link,
    axis: AxisCalibration,
    axisIndex: number, // 0=x 1=y 2=z 3=a
    targetPosMm: number,
    rateMmPerSec: number,
    opts?: JogToOptions,
): JogHandle {
    let sess: Session | null = null;
    const run = (async (): Promise<boolean> => {
        const st = await link.getStatus();
        const wireCurrent = st.pos![axisIndex]!;
        const inv = axis.invert ? -1 : 1;
        const wireTarget = Math.round(targetPosMm * axis.stepsPerUnit * inv);
        const wireDelta = wireTarget - wireCurrent;
        if (wireDelta === 0) return true;

        const feedSps = Math.max(1, rateMmPerSec * axis.stepsPerUnit);
        const accelSps2 = Math.max(rateMmPerSec * 8, 50) * axis.stepsPerUnit;
        const vStart = opts?.vStart ?? V_START_DEFAULT;
        const vec: [number, number, number, number] = [0, 0, 0, 0];
        vec[axisIndex] = wireDelta;
        const packets = makeJog(vec, feedSps, accelSps2, F_CPU, vStart);

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