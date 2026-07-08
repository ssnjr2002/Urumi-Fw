/**
 * microsegment.ts — the MicroSegment wire-event type and its emit helpers.
 * Ported from pipeline/stages/microsegment.py.
 *
 * The bottom-of-pipeline unit: one MicroSegment is one step-timing event
 * (per-axis integer step deltas + a clock interval). The Discretize stage
 * and the Choreograph module both produce these; host/serialise.py (future)
 * packs them to the 26-byte wire format.
 *
 * The interval() helper computes clock cycles per major-axis step so the XY
 * tool moves at v mm/s, with XY hypotenuse correction and per-axis rate
 * limits. It takes a ResolvedAxes slice (4 axes + fCpu) — the caller resolves
 * the head's Z/A via resolvedAxes(machine) from config.ts.
 */

import type { AxisConfig, ResolvedAxes } from "../../config/config.js";

// ── wire event type ───────────────────────────────────────────────────────────

export interface MicroSegment {
    readonly dx: number;       // X steps (signed int)
    readonly dy: number;       // Y steps (signed int)
    readonly dz: number;       // Z steps (signed int)
    readonly da: number;       // A steps (signed int, tangential rotation)
    readonly interval: number; // clock cycles for major axis
    readonly flags: number;    // MICRO_PATH_END etc.
}

export function microSegment(
    dx: number,
    dy: number,
    dz: number,
    da: number,
    interval: number,
    flags = 0,
): MicroSegment {
    return { dx, dy, dz, da, interval, flags };
}

// ── flag constants ────────────────────────────────────────────────────────────
// The flags byte is ONE namespace shared with the wire (see
// docs/wire_protocol.md). Low bits are wire/firmware semantics, high bits are
// host planning hints the firmware ignores:
//   0x01 PATH_END (shared)   0x02 ESTOP (wire)   0x04 PAUSE (wire, sender-inserted)
//   0x08 LIFT (host hint)    0x10 JOG (host hint)
// JOG must NOT be 0x04 — that would alias every travel move onto MSEG_FLAG_PAUSE.

export const MICRO_PATH_END = 0x01;
export const MICRO_PAUSE    = 0x04; // sender-inserted at tool-change boundary; firmware → PAUSED after this packet
export const MICRO_LIFT = 0x08;
export const MICRO_JOG = 0x10;

// ── interval helper ───────────────────────────────────────────────────────────

/**
 * Clock cycles per major-axis step so the XY TOOL moves at v mm/s.
 *
 * The Pico times a segment by its major axis (max steps over all driven axes),
 * but the tool travels the XY hypotenuse — longer than the major leg on a
 * diagonal. Without correction the realized tool speed overshoots v by up to
 * sqrt(2). Scaling the interval by hypot(dx,dy)/major restores the commanded
 * feed; for a pure axis move hypot == major and it reduces to the plain
 * major-axis rate.
 *
 * Called without dx/dy it governs the major axis directly at v (legacy path).
 *
 * Per-axis: the XY tool distance is hypot(dx/x_spu, dy/y_spu), so X and Y may
 * have different resolutions (non-square machine). A per-axis rate limit
 * floors the segment time so no axis exceeds max_rate_i * steps_per_unit_i —
 * this is what keeps the A axis within its slew rate on tight curves.
 */
export function interval(
    v: number,
    axes: ResolvedAxes,
    vMin: number,
    dx?: number,
    dy?: number,
    dz = 0,
    da = 0,
): number {
    const vv = Math.max(v, vMin);
    const xSpu = axes.x.stepsPerUnit;
    const ySpu = axes.y.stepsPerUnit;
    const fCpu = axes.fCpu;

    function majorRate(): number {
        const stepRate = vv * xSpu;
        if (stepRate < 1e-6) return fCpu;
        return Math.max(1, Math.min(Math.trunc(fCpu / stepRate), fCpu));
    }

    if (dx === undefined || dy === undefined) {
        return majorRate();
    }

    const major = Math.max(Math.abs(dx), Math.abs(dy), Math.abs(dz), Math.abs(da));
    if (major === 0) return fCpu;

    // per-axis rate floor: no axis may exceed maxRate * stepsPerUnit
    let tRate = 0;
    const axisEntries: readonly [number, AxisConfig][] = [
        [dx, axes.x],
        [dy, axes.y],
        [dz, axes.z],
        [da, axes.a],
    ];
    for (const [d, ax] of axisEntries) {
        const R = ax.maxRate * ax.stepsPerUnit;
        if (R > 0 && d !== 0) {
            tRate = Math.max(tRate, Math.abs(d) / R);
        }
    }

    const distMm = Math.hypot(dx / xSpu, dy / ySpu); // true XY tool distance (mm)
    if (distMm < 1e-9) {
        // pure rotation / Z move — no XY feed to govern; use the rate floor if any
        if (tRate > 0) {
            const cycles = (tRate / major) * fCpu;
            return Math.max(1, Math.min(Math.trunc(cycles), fCpu));
        }
        return majorRate();
    }

    const segTime = Math.max(distMm / vv, tRate); // feed time, floored by axis rates
    const cycles = (segTime / major) * fCpu;      // per major-axis step
    return Math.max(1, Math.min(Math.trunc(cycles), fCpu));
}
