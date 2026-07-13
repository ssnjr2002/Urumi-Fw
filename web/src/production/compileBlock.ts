/**
 * compileBlock.ts — mm subpaths + a tool → MicroSegment[] (the stage 3-8 chain).
 *
 * The living per-block compile: one SVG layer's subpaths (in mm), the tool that
 * cuts them, and the machine/quality config → compiled wire events. Travel /
 * lift / feed resolve from the tool profile then the machine inside discretize,
 * so this layer takes no per-call feed/lift overrides.
 *
 * Output is guarded for byte-for-byte parity: production/tests/bakePlan.test.ts
 * asserts this reproduces the frozen parity harness (tests/svgToPackets.ts),
 * which production/tests/parity.test.ts pins to the Python reference bins. That
 * frees this file to be refactored for clarity as long as the bytes hold — the
 * harness is the thing that must not move, not this.
 *
 * Two config-bridging subtleties, both parity-critical:
 *   1. A-axis: constrain gets aRate/aAccel ONLY when the tool is tangential
 *      (else 0); plan reads the A accel directly, always. This asymmetry is
 *      deliberate.
 *   2. XY acceleration is a single scalar (machine.x.accel) in the cornering
 *      constraint — a square-machine assumption (x.accel == y.accel). plan(),
 *      by contrast, takes xAccel and yAccel per-axis. See `xyAccel` below.
 */

import type { CubicBezier } from "../toolpath/geometry.js";
import type { MachineConfig, ToolProfile, QualityConfig } from "../config/config.js";
import { resolvedAxes } from "../config/config.js";
import { enforceC1 } from "../toolpath/repair.js";
import { flatten } from "../toolpath/flatten.js";
import { constrain } from "../toolpath/constrain.js";
import { plan } from "../toolpath/plan.js";
import { discretize } from "../toolpath/discretize.js";
import type { MicroSegment } from "../wire/microsegment.js";

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

    // The single XY linear-acceleration ceiling used by the cornering
    // constraint. NOTE: the stage option field is named `aMax` ("accel max"),
    // which reads confusingly next to the A-*axis* params — it is NOT the A
    // axis. Sourced from X on a square-machine assumption (x.accel == y.accel).
    const xyAccel = machine.x.accel;

    // A-axis constraints apply only for a tangential tool; a non-tangential
    // tool (pen, revolver) has A doing slot/orientation, not tangent tracking.
    const tangential = profile.tangential;
    const cornerStop = tangential ? profile.cornerAngleDeg : undefined;
    const aRate = tangential ? axes.a.maxRate : 0;
    const aAccel = tangential ? axes.a.accel : 0;

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
    });

    // Stage 5: constrain — per-sample velocity ceiling
    const constrained = constrain(samples, {
        feedMax: profile.feedMax,
        aMax: xyAccel,
        junctionDeviation: quality.junctionDeviation,
        aRateDegS: aRate,
        aAccelDegS2: aAccel,
        cornerStopAngleDeg: cornerStop,
    });

    // Stage 6: plan — look-ahead feedrate, per-axis accel (A accel always read
    // directly here, unlike the tangential-gated form constrain gets above)
    const planned = plan(constrained, {
        xAccel: machine.x.accel,
        yAccel: machine.y.accel,
        aAccelDegS2: axes.a.accel,
        aMax: xyAccel,
    });

    // Stage 8: discretize — Sample[] → MicroSegment[], choreograph at transitions
    const segments = discretize(planned, machine, profile, quality);
    return { segments, startSteps };
}
