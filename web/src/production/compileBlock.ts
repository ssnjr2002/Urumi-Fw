/**
 * compileBlock.ts — mm subpaths + a tool → MicroSegment[] (the stage 3-8 chain).
 *
 * The living per-block compile: one SVG layer's subpaths (in mm), the tool that
 * cuts them, and the machine/quality config → compiled wire events. Travel /
 * lift / feed resolve from the tool profile then the machine inside discretize,
 * so this layer takes no per-call feed/lift overrides.
 *
 * Output is guarded by a golden snapshot: test/production/snapshot.test.ts bakes
 * fixtures through this stage chain and asserts byte-for-byte equality against a
 * committed golden .bin. The golden is self-referential (generated from this
 * pipeline), so an intentional byte change is accepted by regenerating it
 * (UPDATE_GOLDEN=1) and reviewing the diff — no external reference pins it.
 *
 * Two config-bridging subtleties, both parity-critical:
 *   1. A-axis: constrain gets aRate/aAccel ONLY when the tool is tangential
 *      (else 0); plan reads the A accel directly, always. This asymmetry is
 *      deliberate.
 *   2. XY acceleration is a single scalar (min of X/Y maxAccel) in the cornering
 *      constraint — a square-machine assumption (x.maxAccel == y.maxAccel).
 *      plan(), by contrast, takes xAccel and yAccel per-axis. See `xyAccel`.
 */

import type { CubicBezier } from "../toolpath/geometry.js";
import type { MachineConfig, ToolProfile, QualityConfig } from "../machine/index.js";
import { resolvedAxes } from "../machine/index.js";
import { resolveTargets } from "../machine/resolve.js";
import { enforceC1 } from "../toolpath/repair.js";
import { flatten } from "../toolpath/flatten.js";
import { constrain } from "../toolpath/constrain.js";
import { plan } from "../toolpath/plan.js";
import { discretize } from "../toolpath/discretize.js";
import type { MicroSegment } from "../wire/format/microsegment.js";
import { scheduleDutyBreaks } from "./dutyBreaks.js";

export interface CompileBlockResult {
    /** Compiled wire events in execution order. */
    readonly segments: MicroSegment[];
    /**
     * XY start position in TRUE machine steps (pre-invert), after the
     * toolOffset shift. Ready for use as Block.startSteps.
     */
    readonly startSteps: { readonly x: number; readonly y: number };
}

/**
 * Translate all bezier control points by (dx, dy) in mm.
 * Used to shift paths from tool-tip space into head-center space by applying
 * -toolOffset before the stage chain.
 */
function shiftSubpaths(
    subpaths: readonly (readonly CubicBezier[])[],
    dx: number,
    dy: number,
): CubicBezier[][] {
    if (dx === 0 && dy === 0) return subpaths as CubicBezier[][];
    return subpaths.map((sp) =>
        sp.map((b) => ({
            p0: { x: b.p0.x + dx, y: b.p0.y + dy },
            p1: { x: b.p1.x + dx, y: b.p1.y + dy },
            p2: { x: b.p2.x + dx, y: b.p2.y + dy },
            p3: { x: b.p3.x + dx, y: b.p3.y + dy },
        })),
    );
}

export function compileBlock(
    subpathsMm: readonly (readonly CubicBezier[])[],
    machine: MachineConfig,
    quality: QualityConfig,
    profile: ToolProfile,
): CompileBlockResult {
    const axes = resolvedAxes(machine);

    // Shift paths by -toolOffset so all baked coordinates are in head-center
    // space. Zero offset is a fast-path no-op (returns the original array).
    const shifted = shiftSubpaths(
        subpathsMm,
        -profile.toolOffset.xOffset,
        -profile.toolOffset.yOffset,
    );

    const p0 = shifted[0]?.[0]?.p0 ?? { x: 0, y: 0 };
    const startSteps = {
        x: Math.round(p0.x * machine.x.stepsPerUnit),
        y: Math.round(p0.y * machine.y.stepsPerUnit),
    };

    // Feed/accel resolution (docs/feed_accel_value_model.md):
    //   pathFeed  — cut target: tool override, else machine baseline.
    //   pathAccel — optional cut-accel cap: tool/machine, else unset (0).
    const targets = resolveTargets(machine, profile);
    const pathFeed = targets.path.feed;
    const pathAccel = targets.path.accel ?? 0;

    // The single XY linear-acceleration ceiling used by the cornering
    // constraint's centripetal cap (`aMax`). The constraint collapses XY accel
    // to ONE scalar, unlike plan() which takes x/y accel per-axis. On a
    // non-square machine (x.maxAccel != y.maxAccel) the safe ceiling is the
    // SMALLER of the two — using the larger would let the weaker axis overshoot
    // on corners it dominates. min() is exact for a square machine, so this
    // stays byte-neutral on the current config.
    const xyAccel = Math.min(machine.x.maxAccel, machine.y.maxAccel);

    // A-axis constraints apply only for a tangential tool; a non-tangential
    // tool (pen, revolver) has A doing slot/orientation, not tangent tracking.
    // A is bounded by its axis ceilings (maxFeed / maxAccel).
    const tangential = profile.tangential;
    const cornerStop = tangential ? profile.cornerAngleDeg : undefined;
    const aRate = tangential ? axes.a.maxFeed : 0;
    const aAccel = tangential ? axes.a.maxAccel : 0;

    // Stage 3: repair — enforce C1 continuity, one enforceC1 per subpath
    const repaired = shifted.map(
        (sp) => enforceC1(sp, { angleTolDeg: quality.angleTol, gapTolMm: quality.gapTol }).repaired,
    );

    // Stage 4: flatten — Bezier subpaths → Sample[]
    const samples = flatten(repaired, {
        chordTol: quality.chordTol,
        dsMax: quality.dsMax,
        dthetaMax: quality.dthetaMax,
        dtMax: quality.dtMax,
        dtMin: quality.dtMin,
        maxRefine: quality.maxRefine,
    });

    // Stage 5: constrain — per-sample velocity ceiling
    const constrained = constrain(samples, {
        feedMax: pathFeed,
        aMax: xyAccel,
        junctionDeviation: quality.junctionDeviation,
        aRateDegS: aRate,
        aAccelDegS2: aAccel,
        cornerStopAngleDeg: cornerStop,
        // The same floor discretize clamps intervals to — constrain must not
        // plan a speed the machine will refuse to execute (audit C1).
        vMin: quality.vMin,
    });

    // Stage 6: plan — look-ahead feedrate, per-axis accel.
    //
    // The A term is gated on `tangential`, the same as constrain's above (audit
    // P4). It used to be passed unconditionally, so a pen — not tracking the
    // tangent at all — had its path acceleration cut by rad(aAccel)/kappa on
    // every curve: 8x on a 5mm arc, for an axis that is not moving.
    //
    // A revolver pen does rotate A, but only between operations, and that
    // motion is emitted by choreograph (preOrient / aMoveTo) against A's own
    // limits — it never rides a cutting segment, so it has no claim on the
    // cutting path's acceleration budget.
    const planned = plan(constrained, {
        xAccel: machine.x.maxAccel,
        yAccel: machine.y.maxAccel,
        aAccelDegS2: aAccel,
        aMax: xyAccel,
        pathAccel,
    });

    // Stage 8: discretize — Sample[] → MicroSegment[], choreograph at transitions
    const segments = discretize(planned, machine, profile, quality);

    // Stage 9: duty breaks — mark enable-line resets for a duty-limited tool.
    // A pure post-pass that ORs flags onto lifts already in the stream, so a
    // tool WITHOUT dutyLimits (every tool but the ultrasonic knife) takes this
    // branch and the output is byte-identical to before this stage existed.
    if (profile.dutyLimits) {
        const marked = scheduleDutyBreaks(
            segments, profile.dutyLimits, axes, machine.fCpu, profile.name,
        );
        return { segments: marked.segments, startSteps };
    }
    return { segments, startSteps };
}
