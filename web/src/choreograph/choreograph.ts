/**
 * choreograph.ts — non-cutting motion emitters (stateless, reusable).
 *
 * Extracted from the choreograph half of pipeline/stages/discretize.py (the
 * closures _z_move, _a_move, _pivot, plus the inline travel jog and A
 * pre-orientation logic). Lifted into its own top-level module because the
 * same non-cutting moves are needed outside the toolpath pipeline — tool
 * changing, path stitching, manual jogging.
 *
 * Stateless + state-explicit: each function takes the current state it needs
 * (aPhys, theta, position) and returns new state. No hidden mutable internals.
 * The caller (discretize, or a future tool-change orchestrator) holds the
 * walk state and calls these at transitions.
 *
 * Dependencies: wire/ for MicroSegment + flags, config/ for ResolvedAxes +
 * ToolProfile (type-only). No toolpath/ dependency — choreograph is a peer,
 * not a toolpath stage.
 */

import type { ResolvedAxes, ToolProfile, ToolHead, OpTarget } from "../config/config.js";
import { angleDelta } from "../toolpath/geometry.js";
import {
    MICRO_JOG,
    MICRO_LIFT,
    microSegment,
    interval,
    type MicroSegment,
} from "../wire/format/microsegment.js";

// ── Z lift move (pure Z, constant velocity) ───────────────────────────────────
// TODO: Z moves are currently single-segment constant-velocity (matching the
// Python). A future refinement should ramp Z trapezoidally like aMove —
// extract a generic trapezoidalMove() helper and use it for both A and Z, so
// Z lift/lower doesn't slam at full zFeed. Needs z.accel characterized
// (currently 0 placeholder in defaultConfig).

/**
 * Emit a single Z-axis move at constant velocity (zFeed mm/s).
 * dz is in STEPS (signed). Invert is applied to the emitted dz.
 */
export function zMove(dz: number, axes: ResolvedAxes, zFeed: number): MicroSegment {
    const zRate = Math.max(zFeed * axes.z.stepsPerUnit, 1e-9);
    const zInterval = Math.max(1, Math.min(Math.trunc(axes.fCpu / zRate), axes.fCpu));
    const emittedDz = axes.z.invert ? -dz : dz;
    return microSegment(0, 0, emittedDz, 0, zInterval, MICRO_LIFT);
}

/** Compute the Z step count for a lift of `liftHeight` mm. 0 if no lift. */
export function zStepCount(liftHeight: number, axes: ResolvedAxes): number {
    if (liftHeight <= 0) return 0;
    return Math.round(liftHeight * axes.z.stepsPerUnit);
}

// ── ramped pure-A rotation (trapezoidal) ──────────────────────────────────────

/**
 * Emit a ramped pure-A rotation (trapezoidal velocity profile: accel from
 * v0 → cruise → decel back to v0). Never slams the A axis.
 *
 * da is in STEPS (signed). Invert is applied to the emitted da.
 * Returns [] for da=0.
 *
 * Ported from discretize.py's _a_move — trapezoidal formula preserved
 * exactly: v0 = min(cruise, 50), d_acc = (vc²-v0²)/(2·acc), triangular
 * clamp when 2·d_acc > N.
 */
export function aMove(da: number, axes: ResolvedAxes, slew?: OpTarget): MicroSegment[] {
    const N = Math.abs(Math.trunc(da));
    if (N === 0) return [];

    // Standalone-A slew target (machine-owned). Feed/accel unset → the A axis
    // ceiling (which is itself 0 → the legacy 180/2000 emergency floor).
    const aSpd = axes.a.stepsPerUnit;
    const feed = slew?.feed ?? axes.a.maxFeed;
    const rate = slew?.accel ?? axes.a.maxAccel;
    const cruise = Math.max((feed > 0 ? feed : 180) * aSpd, 1);
    const accel = Math.max((rate > 0 ? rate : 2000) * aSpd, 1);
    const v0 = Math.min(cruise, 50);

    const sign = (da > 0 ? 1 : -1) * (axes.a.invert ? -1 : 1);

    let dAcc = (cruise * cruise - v0 * v0) / (2 * accel);
    if (2 * dAcc > N) dAcc = N / 2;

    const out: MicroSegment[] = [];
    let n = 0;
    while (n < N) {
        let v: number;
        if (n < dAcc) {
            v = Math.sqrt(v0 * v0 + 2 * accel * n);
        } else if (n >= N - dAcc) {
            v = Math.sqrt(Math.max(v0 * v0, v0 * v0 + 2 * accel * (N - n)));
        } else {
            v = cruise;
        }
        v = Math.max(v, v0);
        const chunk = Math.min(Math.max(1, Math.trunc(v / 100)), N - n);
        const iv = Math.max(1, Math.min(Math.trunc(axes.fCpu / v), axes.fCpu));
        out.push(microSegment(0, 0, 0, sign * chunk, iv, MICRO_JOG));
        n += chunk;
    }
    return out;
}

// ── lift-pivot-lower ──────────────────────────────────────────────────────────

/**
 * Lift-pivot-lower: raise Z → rotate A by daTrue → lower Z.
 * Z lift is optional (when lift=false, only the A rotation is emitted).
 * daTrue is in STEPS (signed).
 */
export function pivot(
    daTrue: number,
    lift: boolean,
    zSteps: number,
    axes: ResolvedAxes,
    zFeed: number,
    slew?: OpTarget,
): MicroSegment[] {
    const out: MicroSegment[] = [];
    if (lift) out.push(zMove(+zSteps, axes, zFeed));
    out.push(...aMove(daTrue, axes, slew));
    if (lift) out.push(zMove(-zSteps, axes, zFeed));
    return out;
}

// ── travel jog between subpaths ───────────────────────────────────────────────

/**
 * Emit a travel jog from (fromX, fromY) to (toX, toY) in STEPS.
 * Returns null if there's no movement (dx=dy=0).
 * Invert is applied to the emitted dx/dy.
 */
export function travelJog(
    fromX: number,
    fromY: number,
    toX: number,
    toY: number,
    axes: ResolvedAxes,
    vMin: number,
    jogFeed: number,
): MicroSegment | null {
    const dx = Math.round(toX) - Math.round(fromX);
    const dy = Math.round(toY) - Math.round(fromY);
    if (dx === 0 && dy === 0) return null;
    const emittedDx = axes.x.invert ? -dx : dx;
    const emittedDy = axes.y.invert ? -dy : dy;
    const iv = interval(jogFeed, axes, vMin, dx, dy, 0, 0);
    return microSegment(emittedDx, emittedDy, 0, 0, iv, MICRO_JOG);
}

// ── A pre-orientation at PATH_START ───────────────────────────────────────────

/**
 * Pre-orient the A axis to the entry tangent before lowering to cut.
 *
 * Two modes (branched on profile.tangential + profile.unwind):
 *   unwind (wired tool): rotate to the ABSOLUTE target (entryTheta * stepsPerDeg),
 *     compensating for accumulated physical rotation. Keeps the cable within
 *     ~one turn.
 *   non-unwind (free-spinning): rotate by the DELTA from currentTheta to
 *     entryTheta.
 *
 * Returns { segments, newAPhys }. If the tool is not tangential, returns
 * empty segments and unchanged aPhys.
 */
export function preOrient(
    entryTheta: number,
    currentTheta: number,
    currentAPhys: number,
    axes: ResolvedAxes,
    profile: ToolProfile,
    slew?: OpTarget,
): { segments: MicroSegment[]; newAPhys: number } {
    if (!profile.tangential) {
        return { segments: [], newAPhys: currentAPhys };
    }

    const aSpd = axes.a.stepsPerUnit;
    let daTrue: number;
    if (profile.unwind) {
        const target = Math.round(entryTheta * aSpd);
        daTrue = target - currentAPhys;
    } else {
        daTrue = Math.round(angleDelta(currentTheta, entryTheta) * aSpd);
    }

    if (daTrue === 0) {
        return { segments: [], newAPhys: currentAPhys };
    }

    const segments = aMove(daTrue, axes, slew);
    return { segments, newAPhys: currentAPhys + daTrue };
}

// ── absolute A move (for A-home + revolver slot selection) ────────────────────

/**
 * Move the A axis to an absolute target angle (in degrees).
 *
 * Computes the signed delta from the current physical A position to the
 * target, then delegates to `aMove` for the ramped trapezoidal motion.
 * Returns { segments, newAPhys } — the caller updates its global A
 * state with newAPhys.
 *
 * Uses for an orchestrator:
 *   - A-home to 0° between blocks: aMoveTo(0, aPhys, axes)
 *   - Revolver slot selection:     aMoveTo(slotOffsets[i], aPhys, axes)
 *
 * The target is an ABSOLUTE angle in degrees (not relative). The
 * physical A position is tracked in integer steps by the caller; this
 * function converts both to steps, takes the difference, and emits a
 * ramped relative move.
 */
export function aMoveTo(
    targetDeg: number,
    currentAPhys: number,
    axes: ResolvedAxes,
    slew?: OpTarget,
): { segments: MicroSegment[]; newAPhys: number } {
    const aSpd = axes.a.stepsPerUnit;
    const targetSteps = Math.round(targetDeg * aSpd);
    const daTrue = targetSteps - currentAPhys;
    if (daTrue === 0) {
        return { segments: [], newAPhys: currentAPhys };
    }
    const segments = aMove(daTrue, axes, slew);
    return { segments, newAPhys: currentAPhys + daTrue };
}

// ── head-offset compensation jog ──────────────────────────────────────────────

/**
 * Emit an XY jog to compensate for head offset when switching from one
 * head to another. The machine must move by `(to - from)` so the new
 * head's center is where the old head's center was.
 *
 * The offsets are in mm; the jog is emitted in steps (with invert
 * applied). Returns null if the two heads have the same offset (no
 * compensation needed).
 *
 * The caller (orchestrator) emits this AFTER a tool-change pause and
 * BEFORE the travel jog to the next block's start. It does NOT depend
 * on the machine's current XY position — it's a pure relative shift.
 */
export function headOffsetJog(
    fromHead: ToolHead,
    toHead: ToolHead,
    axes: ResolvedAxes,
    vMin: number,
    jogFeed: number,
): MicroSegment | null {
    const dxMm = toHead.xOffset - fromHead.xOffset;
    const dyMm = toHead.yOffset - fromHead.yOffset;
    if (Math.abs(dxMm) < 1e-9 && Math.abs(dyMm) < 1e-9) return null;

    const dxSteps = Math.round(dxMm * axes.x.stepsPerUnit);
    const dySteps = Math.round(dyMm * axes.y.stepsPerUnit);
    if (dxSteps === 0 && dySteps === 0) return null;

    const emittedDx = axes.x.invert ? -dxSteps : dxSteps;
    const emittedDy = axes.y.invert ? -dySteps : dySteps;
    const iv = interval(jogFeed, axes, vMin, dxSteps, dySteps, 0, 0);
    return microSegment(emittedDx, emittedDy, 0, 0, iv, MICRO_JOG);
}
