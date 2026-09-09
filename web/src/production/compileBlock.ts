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
import { axesForHead } from "../machine/index.js";
import { resolveTargets } from "../machine/resolve.js";
import { enforceC1 } from "../toolpath/repair.js";
import { flatten } from "../toolpath/flatten.js";
import { constrain } from "../toolpath/constrain.js";
import { plan } from "../toolpath/plan.js";
import { discretize, type DiscretizeReport } from "../toolpath/discretize.js";
import type { MicroSegment } from "../wire/format/microsegment.js";
import { scheduleDutyBreaks } from "./dutyBreaks.js";

/**
 * One layer's work, before compiling: a tool, the geometry it cuts, and for a
 * revolver the slot that geometry belongs to.
 *
 * Lives here, next to the function that consumes it, because "a block" only
 * means anything in relation to the compile. `slot` stays out of compileBlock's
 * arguments — the compiler never reads it, so threading it through just to have
 * it echoed back would be ceremony.
 */
export interface Block {
    readonly profile: ToolProfile;
    readonly slot?: number;
    readonly subpaths: readonly (readonly CubicBezier[])[];
}

export interface CompileBlockResult {
    /** Compiled wire events in execution order. */
    readonly segments: MicroSegment[];
    /**
     * XY start position in TRUE machine steps (pre-invert), after the
     * toolOffset shift. Ready for use as CompiledBlock.startSteps.
     */
    readonly startSteps: { readonly x: number; readonly y: number };
}

/**
 * A block compiled against a specific head.
 *
 * Separate from `Block` rather than a `Block` with optional fields, so the type
 * system polices the boundary this redesign exists to enforce: an uncompiled
 * block cannot reach the walk, and a compiled one cannot be re-headed. `head`
 * is not advice — the segments were discretised against that head's Z/A
 * calibration and mean nothing anywhere else.
 */
export interface CompiledBlock {
    readonly profile: ToolProfile;
    readonly slot?: number;
    /** Index into machine.heads. The head these segments were resolved for. */
    readonly head: number;
    readonly segments: readonly MicroSegment[];
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

/**
 * Compile one block's geometry against ONE head.
 *
 * `head` is required, not defaulted. mm becomes steps here, and Z/A
 * stepsPerUnit, invert, maxFeed and maxAccel are all per-head — the trajectory
 * is shaped by all four, so this is not a scale factor that could be applied at
 * the wire afterwards. Defaulting it would put the old bug back: on the bench
 * machine head 0's Z is 1200 steps/mm and head 1's is 600, so a block compiled
 * against the wrong head is a clean 2x error with no exception and no
 * wrong-looking number.
 */
export function compileBlock(
    subpathsMm: readonly (readonly CubicBezier[])[],
    machine: MachineConfig,
    quality: QualityConfig,
    profile: ToolProfile,
    head: number,
): CompileBlockResult {
    const axes = axesForHead(machine, head);

    // Shift paths by -toolOffset so all baked coordinates are in head-center
    // space. Zero offset is a fast-path no-op (returns the original array).
    const shifted = shiftSubpaths(
        subpathsMm,
        -profile.toolOffset.xOffset,
        -profile.toolOffset.yOffset,
    );

    const p0 = shifted[0]?.[0]?.p0 ?? { x: 0, y: 0 };
    // X/Y are the shared gantry and identical on every head, which is why the
    // head only ever changes Z and A. The head-to-head XY offset is a jog the
    // orchestrator emits, not something baked into a block's coordinates.
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
    //
    // Run inside a descending fixed-point loop with 6 and 8, because the A-slew
    // ceiling constrain applies is computed against the CONTINUOUS curvature κ
    // while `interval()` enforces it against the QUANTISED |da|/dist. Where they
    // disagree the packet silently runs slower than planned. Each pass measures
    // what packet space allowed and hands it back as an extra ceiling; ceilings
    // only ever fall, so the loop descends and terminates. See
    // ConstrainOptions.measuredCeilings and docs/planner_spaces.md §5.3.
    //
    // Byte-neutral for a non-tangential tool: with aRate = 0 the A floor is off,
    // the XY floors sit above the planned feed, and pass 1 finds nothing to lower.
    const constrainOpts = {
        feedMax: pathFeed,
        aMax: xyAccel,
        junctionDeviation: quality.junctionDeviation,
        aRateDegS: aRate,
        aAccelDegS2: aAccel,
        cornerStopAngleDeg: cornerStop,
        // The same floor discretize clamps intervals to — constrain must not
        // plan a speed the machine will refuse to execute (audit C1).
        vMin: quality.vMin,
    };
    const constrained = constrain(samples, constrainOpts);

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
    const planOpts = {
        xAccel: machine.x.maxAccel,
        yAccel: machine.y.maxAccel,
        aAccelDegS2: aAccel,
        aMax: xyAccel,
        pathAccel,
    };
    let planned = plan(constrained, planOpts);

    // Stage 8: discretize — Sample[] → MicroSegment[], choreograph at transitions
    const report: DiscretizeReport = { vCap: [] };
    let segments = discretize(planned, machine, axes, profile, quality, undefined, report);

    // ── the feedback passes ───────────────────────────────────────────────────
    //
    // Bounded rather than run to convergence: re-planning slower changes the
    // subdivision, which changes |da| per sub-segment, which moves the measured
    // cap slightly — so the sequence descends towards a fixed point rather than
    // landing on one. The first pass removes essentially all of the error and
    // each further pass is strictly safe (it can only lower), so a small cap
    // buys determinism and a bounded compile time. Exceeding it is not an error:
    // the result is a valid, conservatively-planned stream either way.
    const MAX_FEEDBACK_PASSES = 3;
    const held = new Array<number>(samples.length).fill(Infinity);
    for (let pass = 0; pass < MAX_FEEDBACK_PASSES; pass++) {
        let lowered = false;
        for (let i = 0; i < samples.length; i++) {
            const cap = report.vCap[i] ?? Infinity;
            if (cap < held[i]!) held[i] = cap;
            // Only a cap that actually binds the CURRENT plan is progress. A cap
            // above the planned speed is the floor sitting idle, which is the
            // state we are driving towards, not evidence to act on.
            if (held[i]! < planned[i]!.v - 1e-9) lowered = true;
        }
        if (!lowered) break;
        const reC = constrain(samples, { ...constrainOpts, measuredCeilings: held });
        planned = plan(reC, planOpts);
        report.vCap = [];
        segments = discretize(planned, machine, axes, profile, quality, undefined, report);
    }

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
