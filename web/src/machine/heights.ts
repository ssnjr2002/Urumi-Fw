/**
 * heights.ts — where a tool cuts and where it is clear, for one job.
 *
 * Heights are mm above the mat (docs/tool_probe_planner_integration.md §1):
 *
 *   cut   = plunge ? 0 : materialMm
 *   clear = materialMm + clearanceMm
 *   lift  = clear − cut
 *
 * The planner only needs `lift` (blocks are relative). The runner turns
 * `clear` into an absolute Z from the height the Pico stores (`setprobe`):
 * the switch contact for a Z with a probe block, the operator's touch-off on
 * the mat for one without.
 */

import type { AxisConfig, MachineConfig, ToolProfile } from "./schema.js";

export interface ToolHeights {
    readonly cutMm: number;
    readonly clearMm: number;
    readonly liftMm: number;
}

export function toolHeights(
    profile: ToolProfile,
    machine: MachineConfig,
    materialMm: number,
): ToolHeights {
    if (!Number.isFinite(materialMm) || materialMm < 0) {
        throw new RangeError(`materialMm must be a number >= 0 (got ${materialMm})`);
    }
    const cutMm = profile.plunge ? 0 : materialMm;
    const clearMm = materialMm + machine.clearanceMm;
    return { cutMm, clearMm, liftMm: clearMm - cutMm };
}

/**
 * Wire-frame sign of "toward the bed" for a Z axis. Z counts up toward the bed
 * (the top switch is the origin), and `invert` flips the wire.
 */
export function zDownSign(z: AxisConfig): 1 | -1 {
    return z.invert ? -1 : 1;
}

/**
 * Absolute wire-frame Z, in steps, at `heightMm` above the mat, from the height
 * the Pico stores for this Z.
 */
export function zAtHeightSteps(storedSteps: number, heightMm: number, z: AxisConfig): number {
    const down = zDownSign(z);
    const matSteps = storedSteps + down * (z.probe?.tripMm ?? 0) * z.stepsPerUnit;
    return Math.round(matSteps - down * heightMm * z.stepsPerUnit);
}
