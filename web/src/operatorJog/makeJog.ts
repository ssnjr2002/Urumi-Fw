/**
 * operatorJog/makeJog.ts — trapezoidal jog as a list of JOG packets.
 * Ported from host/protocol/packets.py make_jog.
 *
 * Given a signed step vector (sx, sy, sz, sa), a feed rate in major-axis steps
 * per second, an acceleration in steps/s^2, the CPU clock (used to convert
 * speeds to the Pico's step interval in CPU cycles), and an optional rest
 * velocity, produces a list of 26-byte JOG packets (magic 0xAE) that ramp up
 * from rest/fStart, cruise at feedSps, and ramp down to rest.
 *
 * Steps are packed into ~10 ms chunks so the packet count stays small
 * regardless of step count — a 10800-step A move becomes ~25 packets instead
 * of 10800. The Pico's Bresenham loop handles multi-step deltas identically
 * to single-step ones.
 *
 * Velocity for each chunk follows v = sqrt(v0^2 + 2*a*d) on the acceleration
 * side and the mirror on the deceleration side, producing a smooth trapezoidal
 * ramp. Minor axes track the major axis via Bresenham accumulators so the
 * spatial path is a straight line in step space.
 */

import {
    packJog,
} from "../wire/format/packet.js";
import {
    microSegment,
    MICRO_PATH_END,
} from "../wire/format/microsegment.js";

/**
 * Build a trapezoidal jog burst.
 *
 * @param steps  — [sx, sy, sz, sa] signed target step counts. Must contain
 *                 at least one non-zero element (the major axis).
 * @param feedSps  — cruise velocity of the major axis in steps/sec.
 * @param accelSps2 — acceleration of the major axis in steps/sec^2.
 * @param fCpu    — the Pico's CPU clock (the step interval in the packet
 *                  is `fCpu / v_current`).
 * @param vStartSps — velocity at the start of the move (usually 50 steps/s,
 *                    the minimum the machine can maintain without stalling).
 */
export function makeJog(
    steps: readonly [number, number, number, number],
    feedSps: number,
    accelSps2: number,
    fCpu: number,
    vStartSps: number = 50,
): Uint8Array[] {
    const absSteps = steps.map((s) => Math.abs(s)) as [number, number, number, number];
    const major = Math.max(...absSteps);
    if (major === 0) return [];

    const signs = steps.map((s) => (s >= 0 ? 1 : -1)) as [number, number, number, number];
    const v0 = Math.max(1, Math.min(vStartSps, feedSps));

    // Trapezoid geometry in major-axis steps
    let dAcc = (feedSps * feedSps - v0 * v0) / (2 * accelSps2);
    if (2 * dAcc > major) {
        // triangular — never reach cruise
        const peak = Math.sqrt(v0 * v0 + accelSps2 * major);
        dAcc = (peak * peak - v0 * v0) / (2 * accelSps2);
    }
    const dDec = dAcc;

    const err = [Math.trunc(major / 2), Math.trunc(major / 2), Math.trunc(major / 2), Math.trunc(major / 2)];
    const packets: Uint8Array[] = [];
    let n = 0;

    while (n < major) {
        // velocity at the start of this chunk
        let v: number;
        if (n < dAcc) {
            v = Math.sqrt(v0 * v0 + 2 * accelSps2 * n);
        } else if (n >= major - dDec) {
            v = Math.sqrt(v0 * v0 + 2 * accelSps2 * (major - n));
        } else {
            v = feedSps;
        }
        v = Math.max(v, v0);

        // Adaptive chunk: ~10 ms at the current velocity. Small during
        // accel/decel so the interval is accurate; large at cruise for
        // streaming efficiency.
        const chunkSize = Math.min(Math.max(1, Math.trunc(v / 100)), major - n);
        const interval = Math.max(1, Math.min(Math.trunc(fCpu / v), fCpu));

        // Per-axis deltas for this chunk via Bresenham
        const delta: [number, number, number, number] = [0, 0, 0, 0];
        for (let ax = 0; ax < 4; ax++) {
            if (absSteps[ax] === 0) continue;
            if (absSteps[ax] === major) {
                delta[ax] = signs[ax]! * chunkSize;
            } else {
                let count = 0;
                let e = err[ax]!;
                for (let _ = 0; _ < chunkSize; _++) {
                    e += absSteps[ax]!;
                    if (e >= major) {
                        e -= major;
                        count++;
                    }
                }
                err[ax] = e;
                delta[ax] = signs[ax]! * count;
            }
        }

        n += chunkSize;
        const flags = n >= major ? MICRO_PATH_END : 0;
        packets.push(
            packJog(
                microSegment(delta[0], delta[1], delta[2], delta[3], interval, flags),
            ),
        );
    }

    return packets;
}