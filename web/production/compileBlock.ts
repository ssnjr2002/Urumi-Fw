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

import type { CubicBezier } from "../toolpath/src/geometry.js";
import type { MachineConfig, ToolProfile, QualityConfig } from "../config/config.js";
import { resolvedAxes } from "../config/config.js";
import { enforceC1 } from "../toolpath/src/repair.js";
import { flatten } from "../toolpath/src/flatten.js";
import { constrain } from "../toolpath/src/constrain.js";
import { plan } from "../toolpath/src/plan.js";
import { discretize } from "../toolpath/src/discretize.js";
import type { MicroSegment } from "../wire/src/microsegment.js";

export function compileBlock(
    subpathsMm: readonly (readonly CubicBezier[])[],
    machine: MachineConfig,
    quality: QualityConfig,
    profile: ToolProfile,
): MicroSegment[] {
    const axes = resolvedAxes(machine);

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
    const repaired = subpathsMm.map(
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
    return discretize(planned, machine, profile, quality);
}
