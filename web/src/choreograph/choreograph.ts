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

// ── trapezoidal ramp generator (shared by A rotation and XY jogs) ─────────────

/** One emitted piece of a ramp: `steps` major-axis steps clocked at `interval`. */
export interface RampChunk {
    readonly steps: number;
    readonly interval: number;
}

/**
 * How many pieces each ramp is cut into. The ramp is exact at every chunk
 * BOUNDARY regardless of this number — it only sets how finely the speed
 * staircase approximates the continuous ramp, and so how much the axis is
 * asked to jerk at each boundary. 16 keeps a full-speed ramp under ~1/16th of
 * a step change per boundary while costing ~32 segments for the whole move.
 */
const RAMP_CHUNKS = 16;

/**
 * Cut a pure single-axis move of `N` steps into a trapezoidal speed profile:
 * accelerate v0 → peak, cruise, decelerate peak → v0, never exceeding `accel`.
 *
 * The interval of each chunk is derived from the EXACT time that chunk takes
 * under constant acceleration — dt = |v_end - v_start| / accel — not from the
 * speed sampled at one end of it. That distinction is the whole of audit H1:
 * sampling at the chunk START is the slowest point of an accelerating chunk
 * (conservative) and the FASTEST point of a decelerating one (anti-conservative
 * by 1.26-1.65x), which is one line producing an error of opposite sign on the
 * two halves of the same move. A mean derived from the kinematics has no side.
 *
 * All speeds are in steps/s, `accel` in steps/s^2. Chunk boundaries are integer
 * step counts, so the emitted move is exactly N steps.
 */
export function rampChunks(
    N: number,
    v0: number,
    cruise: number,
    accel: number,
    fCpu: number,
): RampChunk[] {
    if (N <= 0) return [];

    // Ramp length, triangular-clamped when there is no room to reach cruise.
    let dAcc = (cruise * cruise - v0 * v0) / (2 * accel);
    if (2 * dAcc > N) dAcc = N / 2;
    const peak = Math.sqrt(v0 * v0 + 2 * accel * dAcc);

    /** Speed of the continuous profile at step distance n. */
    const vAt = (n: number): number => {
        if (n <= dAcc) return Math.sqrt(v0 * v0 + 2 * accel * n);
        if (n >= N - dAcc) return Math.sqrt(Math.max(v0 * v0, v0 * v0 + 2 * accel * (N - n)));
        return peak;
    };

    // Boundaries: equal speed increments up the ramp, one piece across the
    // cruise (dv = 0 there, so a single chunk is already exact), mirrored down.
    const marks = new Set<number>([0, N]);
    const dv = (peak - v0) / RAMP_CHUNKS;
    if (dv > 0) {
        for (let i = 1; i <= RAMP_CHUNKS; i++) {
            const v = v0 + i * dv;
            const n = (v * v - v0 * v0) / (2 * accel);
            marks.add(Math.min(Math.round(n), N));
            marks.add(Math.max(N - Math.round(n), 0));
        }
    }
    const bounds = [...marks].sort((p, q) => p - q);

    const out: RampChunk[] = [];
    for (let i = 0; i + 1 < bounds.length; i++) {
        const a = bounds[i]!;
        const b = bounds[i + 1]!;
        const steps = b - a;
        if (steps <= 0) continue;
        const vA = vAt(a);
        const vB = vAt(b);
        // Exact duration: constant-accel over the chunk, or constant speed when
        // the two ends agree (the cruise piece, and any degenerate ramp piece).
        const dt = Math.abs(vB - vA) > 1e-9
            ? Math.abs(vB - vA) / accel
            : steps / Math.max(vA, 1e-9);
        const iv = Math.max(1, Math.min(Math.round((fCpu * dt) / steps), fCpu));
        out.push({ steps, interval: iv });
    }
    return out;
}

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

    // Standalone-A slew target (machine-owned), falling back to the A axis
    // ceiling. A 0 ceiling means "uncapped" everywhere else in the config, but
    // a trapezoid cannot be built from "uncapped" — it needs an actual speed.
    // This used to substitute 180 deg/s and 2000 deg/s^2 silently, which made
    // an undeclared A axis run FASTER than a declared one (audit H4). load.ts
    // refuses to invent stepsPerUnit/invert for the same reason; refuse here
    // too, and say which knob is missing.
    const aSpd = axes.a.stepsPerUnit;
    const feed = slew?.feed ?? axes.a.maxFeed;
    const rate = slew?.accel ?? axes.a.maxAccel;
    if (!(feed > 0) || !(rate > 0)) {
        const missing = [!(feed > 0) ? "feed" : null, !(rate > 0) ? "accel" : null]
            .filter(Boolean)
            .join(" and ");
        throw new Error(
            `aMove: cannot rotate A by ${N} steps — no ${missing} limit. Set ` +
            `machine.heads[].a.maxFeed/maxAccel, or pass an explicit slew target.`,
        );
    }
    const cruise = Math.max(feed * aSpd, 1);
    const accel = Math.max(rate * aSpd, 1);
    const v0 = Math.min(cruise, 50);

    const sign = (da > 0 ? 1 : -1) * (axes.a.invert ? -1 : 1);

    return rampChunks(N, v0, cruise, accel, axes.fCpu).map((c) =>
        microSegment(0, 0, 0, sign * c.steps, c.interval, MICRO_JOG),
    );
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
 * Emit a straight XY move of (dx, dy) STEPS as a ramped travel jog.
 *
 * Shared by `travelJog` and `headOffsetJog`: both used to emit ONE segment at
 * full jog feed, which asks the machine for its whole travel speed in zero
 * distance — 0 → 80 mm/s instantly, against a configured `x.maxAccel` of 1000
 * mm/s² that the cutting path respects everywhere (audit H2). The step totals
 * are unchanged; only the timeline is.
 *
 * The per-axis deltas are distributed proportionally with float accumulators
 * rounded at emit, so the chunks sum to exactly (dx, dy) with no drift.
 */
function xyJog(
    dx: number,
    dy: number,
    axes: ResolvedAxes,
    vMin: number,
    jogFeed: number,
): MicroSegment[] {
    if (dx === 0 && dy === 0) return [];

    // Cruise speed comes from interval() exactly as before, so the jog's top
    // speed and every per-axis feed floor keep their existing meaning.
    const ivCruise = interval(jogFeed, axes, vMin, dx, dy, 0, 0);
    const major = Math.max(Math.abs(dx), Math.abs(dy));
    const cruise = axes.fCpu / ivCruise; // major-axis steps/s

    // XY acceleration ceiling, converted from mm/s^2 to major-axis steps/s^2
    // along THIS path. Whichever axis is tighter owns the move.
    const lenMm = Math.hypot(dx / axes.x.stepsPerUnit, dy / axes.y.stepsPerUnit);
    const accelMm = Math.min(
        axes.x.maxAccel > 0 ? axes.x.maxAccel : Infinity,
        axes.y.maxAccel > 0 ? axes.y.maxAccel : Infinity,
    );
    if (!(accelMm > 0) || !Number.isFinite(accelMm) || lenMm <= 0) {
        // No declared XY accel means there is nothing to ramp against. Refuse
        // rather than silently slam, the same policy aMove uses for A (H4).
        throw new Error(
            `travel jog of ${major} steps: no XY acceleration limit. Set ` +
            "machine.x.maxAccel and machine.y.maxAccel.",
        );
    }
    const accel = (accelMm * major) / lenMm;

    // Junction speed: the same standstill-ish entry/exit aMove uses, so a jog
    // starts and ends slow instead of at feed.
    const v0 = Math.min(cruise, 50);

    const chunks = rampChunks(major, v0, cruise, accel, axes.fCpu);
    const out: MicroSegment[] = [];
    let doneMajor = 0;
    let accX = 0;
    let accY = 0;
    for (const c of chunks) {
        doneMajor += c.steps;
        const f = doneMajor / major;
        const tgtX = dx * f;
        const tgtY = dy * f;
        const sx = Math.round(tgtX) - Math.round(accX);
        const sy = Math.round(tgtY) - Math.round(accY);
        accX = tgtX;
        accY = tgtY;
        if (sx === 0 && sy === 0) continue;
        out.push(microSegment(
            axes.x.invert ? -sx : sx,
            axes.y.invert ? -sy : sy,
            0,
            0,
            c.interval,
            MICRO_JOG,
        ));
    }
    return out;
}

/**
 * Emit a ramped travel jog from (fromX, fromY) to (toX, toY) in STEPS.
 * Returns [] if there's no movement (dx=dy=0).
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
): MicroSegment[] {
    const dx = Math.round(toX) - Math.round(fromX);
    const dy = Math.round(toY) - Math.round(fromY);
    return xyJog(dx, dy, axes, vMin, jogFeed);
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
 * applied), ramped like any other travel move. Returns [] if the two heads
 * have the same offset (no compensation needed).
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
): MicroSegment[] {
    const dxMm = toHead.xOffset - fromHead.xOffset;
    const dyMm = toHead.yOffset - fromHead.yOffset;
    if (Math.abs(dxMm) < 1e-9 && Math.abs(dyMm) < 1e-9) return [];

    const dxSteps = Math.round(dxMm * axes.x.stepsPerUnit);
    const dySteps = Math.round(dyMm * axes.y.stepsPerUnit);
    return xyJog(dxSteps, dySteps, axes, vMin, jogFeed);
}
