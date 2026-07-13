/**
 * Discretize stage (redesign stage 8): planned Sample stream -> MicroSegments.
 * Ported from pipeline/stages/discretize.py (the discretize half — the
 * choreograph half was extracted to web/choreograph/).
 *
 * The per-pair emit: turns each consecutive planned sample pair into
 * MicroSegments — per-axis integer step deltas (float accumulators, round at
 * emit), tangent-tracking da, and an interval from the planned speed v.
 * Velocity-aware subdivision splits a pair into k sub-segments so the speed
 * never changes by more than dvMax within one MicroSegment (cruise stays k=1;
 * ramps subdivide; corners stay k=1 — handled as a single near-zero step +
 * pivot).
 *
 * At each transition (PATH_START, corner, PATH_END) the stage calls the
 * choreograph module's stateless functions for non-cutting motion: travel jog
 * between subpaths, A pre-orientation + Z-lower at PATH_START, lift-pivot-
 * lower at corners, Z-raise at PATH_END. The walk state (posX, posY, theta,
 * aAccum, aPhys) is local mutable state inside this function — the function is
 * still pure (returns a fresh array, never mutates the input samples).
 *
 * Why this is simpler than the tile-era stage6: velocity planning already
 * brought the tool to v=0 at every corner (Constrain set vCeiling=0, Plan
 * propagated it), so a corner is just "two adjacent samples whose tangent
 * jumps by >= the tool's corner angle". Between-curve corners and in-curve
 * cusps collapse into ONE rule.
 */

import type { MachineConfig, QualityConfig, ToolProfile } from "../config/config.js";
import { needsOffsetComp, resolvedAxes, type ResolvedAxes } from "../config/config.js";
import { angleDelta } from "./geometry.js";
import { subpathRanges, type PlannedSample } from "./plan.js";
import {
    MICRO_PATH_END,
    interval,
    microSegment,
    type MicroSegment,
} from "../wire/microsegment.js";
import {
    zMove,
    zStepCount,
    pivot,
    travelJog,
    preOrient,
} from "../choreograph/choreograph.js";

export interface DiscretizeOverrides {
    readonly jogFeed?: number;
    readonly liftHeight?: number;
    readonly zFeed?: number;
}

/**
 * Walk the planned Sample stream and emit a flat MicroSegment[].
 *
 * samples — PlannedSample[] with v resolved (after Constrain + Plan).
 * machine — MachineConfig (all 4 axes' calibration, fCpu, travel defaults).
 * profile — ToolProfile (PEN/KNIFE/CREASE). Selects tangent tracking, corner
 *           threshold, unwind, lift.
 * quality — QualityConfig (dvMax, vMin for subdivision + interval).
 * overrides — explicit per-call overrides for jogFeed/liftHeight/zFeed;
 *             absence falls back to profile, then machine defaults.
 */
export function discretize(
    samples: readonly PlannedSample[],
    machine: MachineConfig,
    profile: ToolProfile,
    quality: QualityConfig,
    overrides?: DiscretizeOverrides,
): MicroSegment[] {
    if (needsOffsetComp(profile)) {
        throw new Error(
            `tool profile '${profile.name}' has offsetMm=${profile.offsetMm} ` +
                "(> OFFSET_TOLERANCE_MM); blade-offset compensation is not " +
                "implemented. Use a centre-pivot tool until it lands.",
        );
    }

    const axes: ResolvedAxes = resolvedAxes(machine);
    const tangential = profile.tangential;
    const cornerAngle = profile.cornerAngleDeg;

    // Resolve travel defaults: overrides > profile > machine
    const jogFeed = overrides?.jogFeed ?? (profile.jogFeed > 0 ? profile.jogFeed : machine.jogFeed);
    const liftHeight = overrides?.liftHeight ?? profile.liftHeight;
    const zFeed = overrides?.zFeed ?? (profile.zFeed > 0 ? profile.zFeed : machine.zFeed);

    const xSpu = axes.x.stepsPerUnit;
    const ySpu = axes.y.stepsPerUnit;
    const aSpd = axes.a.stepsPerUnit;

    const zSteps = zStepCount(liftHeight, axes);
    const lift = zSteps > 0;

    const out: MicroSegment[] = [];
    let posX = 0;   // float step accumulators (round at emit)
    let posY = 0;
    let theta = 0;           // logical current tangent (deg)
    let aAccum = 0;          // float A steps (tracking; telescopes to exact net)
    let aPhys = 0;           // physical A steps (TRUE rotation; for unwind)
    let started = false;

    for (const [lo, hi] of subpathRanges(samples)) {
        const first = samples[lo]!;
        const targetX = first.x * xSpu;
        const targetY = first.y * ySpu;

        // travel jog from previous subpath's end
        if (started) {
            const jog = travelJog(posX, posY, targetX, targetY, axes, quality.vMin, jogFeed);
            if (jog) out.push(jog);
        }

        // A pre-orientation to the entry tangent (pen-up), incl. unwind
        const entryTheta = first.theta;
        const orient = preOrient(entryTheta, theta, aPhys, axes, profile);
        out.push(...orient.segments);
        aPhys = orient.newAPhys;

        posX = targetX;
        posY = targetY;
        theta = entryTheta;
        aAccum = aPhys;
        started = true;

        if (lift) out.push(zMove(-zSteps, axes, zFeed)); // lower to cut

        // ── walk the cutting samples ──────────────────────────────────────────
        for (let i = lo; i < hi; i++) {
            const a = samples[i]!;
            const b = samples[i + 1]!;
            const dtheta = angleDelta(theta, b.theta);
            const isCorner = tangential && Math.abs(dtheta) >= cornerAngle;
            const final = i + 1 === hi;

            // Velocity-aware subdivision (premortem P3): split the pair into k
            // sub-segments so the speed never changes by more than dvMax within
            // one MicroSegment. Cruise (dv~0) stays k=1; only ramps subdivide.
            // Corners (v~0 both ends, dtheta huge) also stay k=1.
            let k: number;
            if (isCorner) {
                k = 1;
            } else {
                k = Math.max(1, Math.ceil(Math.abs(b.v - a.v) / quality.dvMax));
                k = Math.min(k, 256);
            }

            const baseX = posX;
            const baseY = posY;
            let thPrev = 0;
            for (let j = 1; j <= k; j++) {
                const f = j / k;
                const tgtX = baseX + (b.x - a.x) * xSpu * f;
                const tgtY = baseY + (b.y - a.y) * ySpu * f;
                const dx = Math.round(tgtX) - Math.round(posX);
                const dy = Math.round(tgtY) - Math.round(posY);

                const thF = theta + dtheta * f;
                let da = 0;
                if (tangential && !isCorner) {
                    const refTheta = j === 1 ? theta : thPrev;
                    const aNew = aAccum + (thF - refTheta) * aSpd;
                    da = Math.round(aNew) - Math.round(aAccum);
                    aAccum = aNew;
                }
                thPrev = thF;

                const lastSub = j === k;
                const segFinal = final && lastSub;

                // skip zero-motion interior sub-steps (don't drop the final/corner)
                if (!segFinal && !(isCorner && lastSub) && dx === 0 && dy === 0 && da === 0) {
                    posX = tgtX;
                    posY = tgtY;
                    continue;
                }

                const v0 = a.v + (b.v - a.v) * ((j - 1) / k);
                const v1 = a.v + (b.v - a.v) * f;
                const vbar = 0.5 * (v0 + v1);
                const iv = interval(vbar, axes, quality.vMin, dx, dy, 0, da);
                const flags = segFinal ? MICRO_PATH_END : 0;
                out.push(microSegment(
                    axes.x.invert ? -dx : dx,
                    axes.y.invert ? -dy : dy,
                    0,
                    axes.a.invert ? -da : da,
                    iv,
                    flags,
                ));
                aPhys += da;
                posX = tgtX;
                posY = tgtY;
            }

            theta = b.theta;

            // lift-pivot-lower at the corner we just arrived at (v is ~0 here)
            if (isCorner) {
                const daTrue = Math.round(dtheta * aSpd);
                if (daTrue !== 0) {
                    out.push(...pivot(daTrue, lift, zSteps, axes, zFeed));
                    aPhys += daTrue;
                }
                aAccum = aPhys;
            }
        }

        if (lift) out.push(zMove(+zSteps, axes, zFeed)); // raise after the stroke
    }

    return out;
}
