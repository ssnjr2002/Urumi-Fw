/**
 * Tests for choreograph — non-cutting motion emitters (stateless).
 *
 * Structure mirrors the rest of the planner audit (see docs/planner_audit.md):
 *
 *   INVARIANTS         must hold for every input, forever. A failure is a bug.
 *   CONTRACT PROPERTIES the behaviour callers are entitled to rely on. Some of
 *                      these are currently RED and name the finding they pin.
 *
 * The kinematic checks deliberately reconstruct the motion the way the
 * FIRMWARE will execute it — |steps| clocked at `interval` cycles apiece —
 * rather than from the velocity the emitter believed it was writing. A check
 * expressed in the emitter's own terms cannot see the emitter's own error;
 * that is how H1 was found.
 */

import { describe, it, expect } from "vitest";
import {
    zMove,
    zStepCount,
    aMove,
    pivot,
    travelJog,
    preOrient,
    aMoveTo,
    headOffsetJog,
} from "../../src/choreograph/choreograph.js";
import {
    MICRO_JOG,
    MICRO_LIFT,
    type MicroSegment,
} from "../../src/wire/format/microsegment.js";
import {
    resolvedAxes,
    axisConfig,
    busNode,
    toolHead,
    KNIFE,
    PEN,
    CREASE,
    type ResolvedAxes,
} from "../../src/config/config.js";
import { defaultConfig } from "../../src/config/fixtures.js";

const axes = resolvedAxes(defaultConfig().machine);

/** The A accel ceiling aMove is working to, in steps/s². */
const A_ACCEL = axes.a.maxAccel * axes.a.stepsPerUnit;
/** The A feed ceiling, in steps/s. */
const A_CRUISE = axes.a.maxFeed * axes.a.stepsPerUnit;

// ── executed-motion reconstruction (firmware's view, not the emitter's) ───────

interface Slice {
    readonly steps: number;
    /** Step rate the firmware will actually clock this slice at. */
    readonly v: number;
    readonly dt: number;
}

/** Major-axis step count of a segment. */
function major(s: MicroSegment): number {
    return Math.max(Math.abs(s.dx), Math.abs(s.dy), Math.abs(s.dz), Math.abs(s.da));
}

function slices(segs: readonly MicroSegment[], ax: ResolvedAxes = axes): Slice[] {
    return segs.map((s) => {
        const steps = major(s);
        return { steps, v: ax.fCpu / s.interval, dt: (steps * s.interval) / ax.fCpu };
    });
}

/**
 * Worst acceleration the emitted staircase demands, as a multiple of `limit`.
 *
 * A slice's rate is its mean over the slice, so it is the speed at the slice's
 * TIME MIDPOINT. The machine therefore has half of each adjacent slice to make
 * the change: the demand is |dv| / ((dt_prev + dt_next) / 2). This asks a
 * question about the emitted bytes alone; it never consults the emitter.
 *
 * This replaced a |dv| / dt_prev convention, which is asymmetric by
 * construction: on an accelerating ramp the LONG slice precedes each boundary
 * and on a decelerating one the SHORT slice does, so it flatters climbs and
 * penalises descents on the very same profile. That was not why H1 was found
 * and it is not why H1 is now closed — measured on the pre-fix emitter, the
 * midpoint convention is strictly HARSHER (worst 2.70x vs 1.65x, and it never
 * drops below 0.74x at any size). Both conventions condemn the old code; only
 * this one is symmetric.
 */
function worstAccelRatio(sl: readonly Slice[], limit: number): number {
    let worst = 0;
    for (let i = 1; i < sl.length; i++) {
        const dt = (sl[i - 1]!.dt + sl[i]!.dt) / 2;
        worst = Math.max(worst, Math.abs(sl[i]!.v - sl[i - 1]!.v) / dt / limit);
    }
    return worst;
}

/** Split at the peak: everything up to it is the accel ramp, after it the decel. */
function rampRatios(sl: readonly Slice[], limit: number): { up: number; down: number } {
    const peak = Math.max(...sl.map((x) => x.v));
    const iPeak = sl.findIndex((x) => x.v === peak);
    return {
        up: worstAccelRatio(sl.slice(0, iPeak + 1), limit),
        down: worstAccelRatio(sl.slice(iPeak), limit),
    };
}

type AxisName = "x" | "y" | "z" | "a";
type AxisPatch = Partial<Omit<Parameters<typeof axisConfig>[2] & object, never>>;

/**
 * The default machine with per-axis field overrides applied. Undefined fields
 * are left alone, so `remap({ a: { maxAccel: 0 } })` changes exactly that.
 */
function remap(patch: Partial<Record<AxisName, AxisPatch | undefined>>): ResolvedAxes {
    const m = defaultConfig().machine;
    const head = m.heads[m.defaultHead]!;
    const clean = (p: AxisPatch | undefined) =>
        Object.fromEntries(Object.entries(p ?? {}).filter(([, v]) => v !== undefined));
    const put = (ax: (typeof m)["x"], p: AxisPatch | undefined) =>
        axisConfig(ax.node, ax.stepsPerUnit, {
            maxFeed: ax.maxFeed,
            maxAccel: ax.maxAccel,
            maxTravel: ax.maxTravel,
            invert: ax.invert,
            rotary: ax.rotary,
            ...clean(p),
        });
    return resolvedAxes({
        ...m,
        x: put(m.x, patch.x),
        y: put(m.y, patch.y),
        heads: [
            toolHead(put(head.z, patch.z), put(head.a, patch.a), {
                xOffset: head.xOffset,
                yOffset: head.yOffset,
            }),
        ],
    });
}

/**
 * Aggregate deltas over a multi-segment emission. Travel jogs are ramped, so
 * their geometry is a property of the SUM, not of any one segment.
 */
function sum(segs: readonly MicroSegment[]): { dx: number; dy: number; dz: number; da: number } {
    return segs.reduce(
        (t, s) => ({ dx: t.dx + s.dx, dy: t.dy + s.dy, dz: t.dz + s.dz, da: t.da + s.da }),
        { dx: 0, dy: 0, dz: 0, da: 0 },
    );
}

/** A tool head at a given offset; axes are irrelevant to head-offset geometry. */
function head(offset: { xOffset: number; yOffset: number }) {
    const m = defaultConfig().machine;
    const h = m.heads[m.defaultHead]!;
    return toolHead(h.z, h.a, offset);
}

/** A range of rotation sizes spanning triangular, short-trapezoid and long. */
const A_SIZES = [52, 129, 258, 500, 1000, 2325, 4650, 9300, 18600];

/** Aggregate over sizes and fail once, so the worst case can't hide. */
function forEachSize(probe: (n: number) => string | null): void {
    const bad = A_SIZES.map(probe).filter((m): m is string => m !== null);
    if (bad.length > 0) expect.fail(`${bad.length}/${A_SIZES.length} sizes:\n  ${bad.join("\n  ")}`);
}

// ─────────────────────────────────────────────────────────────────────────────
// INVARIANTS
// ─────────────────────────────────────────────────────────────────────────────

describe("choreograph INVARIANTS: purity and determinism", () => {
    it("aMove is deterministic", () => {
        expect(aMove(2325, axes)).toEqual(aMove(2325, axes));
    });

    it("emitters do not mutate the axes they are given", () => {
        const before = JSON.stringify(axes);
        aMove(1000, axes);
        zMove(100, axes, 20);
        travelJog(0, 0, 500, 500, axes, 0.5, 80);
        pivot(500, true, 2400, axes, 20);
        preOrient(90, 0, 0, axes, KNIFE);
        aMoveTo(90, 0, axes);
        expect(JSON.stringify(axes)).toBe(before);
    });
});

describe("choreograph INVARIANTS: step conservation", () => {
    it("aMove emits exactly |da| steps, every size", () => {
        forEachSize((n) => {
            const total = aMove(n, axes).reduce((s, x) => s + Math.abs(x.da), 0);
            return total === n ? null : `N=${n}: emitted ${total}`;
        });
    });

    it("aMove emits exactly |da| steps for negative da too", () => {
        forEachSize((n) => {
            const total = aMove(-n, axes).reduce((s, x) => s + Math.abs(x.da), 0);
            return total === n ? null : `N=${-n}: emitted ${total}`;
        });
    });

    it("aMove never emits a zero-motion segment", () => {
        forEachSize((n) => {
            const zeros = aMove(n, axes).filter((s) => s.da === 0).length;
            return zeros === 0 ? null : `N=${n}: ${zeros} zero segments`;
        });
    });

    it("aMoveTo's reported newAPhys matches the steps it emitted", () => {
        for (const [target, from] of [[90, 0], [0, 4650], [-90, 1000], [51.43, -200]]) {
            const r = aMoveTo(target!, from!, axes);
            const emitted = r.segments.reduce((s, x) => s + Math.abs(x.da), 0);
            expect(Math.abs(r.newAPhys - from!)).toBe(emitted);
        }
    });

    it("preOrient's reported newAPhys matches the steps it emitted", () => {
        for (const [entry, cur, phys] of [[90, 0, 0], [0, 90, 4650], [-45, 30, -100]]) {
            for (const tool of [KNIFE, CREASE]) {
                const r = preOrient(entry!, cur!, phys!, axes, tool);
                const emitted = r.segments.reduce((s, x) => s + Math.abs(x.da), 0);
                expect(Math.abs(r.newAPhys - phys!)).toBe(emitted);
            }
        }
    });

    it("pivot conserves Z: the lift and the lower cancel exactly", () => {
        const segs = pivot(1000, true, 2400, axes, 20);
        expect(segs.reduce((s, x) => s + x.dz, 0)).toBe(0);
    });
});

describe("choreograph INVARIANTS: wire encoding", () => {
    it("every emitted interval is in [1, fCpu]", () => {
        const all = [
            ...aMove(18600, axes),
            ...aMove(1, axes),
            ...pivot(1000, true, 2400, axes, 20),
            zMove(2400, axes, 20),
            ...travelJog(0, 0, 32000, 16000, axes, 0.5, 80),
        ];
        for (const s of all) {
            expect(s.interval).toBeGreaterThanOrEqual(1);
            expect(s.interval).toBeLessThanOrEqual(axes.fCpu);
        }
    });

    it("all deltas are integers", () => {
        const all = [
            ...aMove(1234, axes),
            ...pivot(567, true, 2400, axes, 20),
            ...travelJog(0.4, 0.6, 321.7, 89.2, axes, 0.5, 80),
        ];
        for (const s of all) {
            expect(Number.isInteger(s.dx)).toBe(true);
            expect(Number.isInteger(s.dy)).toBe(true);
            expect(Number.isInteger(s.dz)).toBe(true);
            expect(Number.isInteger(s.da)).toBe(true);
            expect(Number.isInteger(s.interval)).toBe(true);
        }
    });

    it("aMove emits pure A motion tagged MICRO_JOG", () => {
        for (const s of aMove(2325, axes)) {
            expect(s.flags).toBe(MICRO_JOG);
            expect([s.dx, s.dy, s.dz]).toEqual([0, 0, 0]);
        }
    });

    it("zMove emits pure Z motion tagged MICRO_LIFT", () => {
        const m = zMove(2400, axes, 20);
        expect(m.flags).toBe(MICRO_LIFT);
        expect([m.dx, m.dy, m.da]).toEqual([0, 0, 0]);
    });

    it("travelJog emits pure XY motion tagged MICRO_JOG", () => {
        const segs = travelJog(0, 0, 3200, 1600, axes, 0.5, 80);
        expect(segs.length).toBeGreaterThan(0);
        for (const m of segs) {
            expect(m.flags).toBe(MICRO_JOG);
            expect([m.dz, m.da]).toEqual([0, 0]);
        }
    });
});

describe("choreograph INVARIANTS: axis inversion", () => {
    /** Same machine, one axis's invert flipped. */
    function withInvert(o: Partial<Record<"x" | "y" | "z" | "a", boolean>>): ResolvedAxes {
        return remap({
            x: { invert: o.x },
            y: { invert: o.y },
            z: { invert: o.z },
            a: { invert: o.a },
        });
    }

    it("flipping z.invert negates every emitted dz and changes nothing else", () => {
        const a = zMove(2400, axes, 20);
        const b = zMove(2400, withInvert({ z: !axes.z.invert }), 20);
        expect(b.dz).toBe(-a.dz);
        expect(b.interval).toBe(a.interval);
    });

    it("flipping a.invert negates every emitted da and changes nothing else", () => {
        const A = aMove(2325, axes);
        const B = aMove(2325, withInvert({ a: !axes.a.invert }));
        expect(B.length).toBe(A.length);
        for (let i = 0; i < A.length; i++) {
            expect(B[i]!.da).toBe(-A[i]!.da);
            expect(B[i]!.interval).toBe(A[i]!.interval);
        }
    });

    it("flipping x.invert negates dx only; y.invert negates dy only", () => {
        const base = sum(travelJog(0, 0, 3200, 1600, axes, 0.5, 80));
        const fx = sum(travelJog(0, 0, 3200, 1600, withInvert({ x: !axes.x.invert }), 0.5, 80));
        const fy = sum(travelJog(0, 0, 3200, 1600, withInvert({ y: !axes.y.invert }), 0.5, 80));
        expect([fx.dx, fx.dy]).toEqual([-base.dx, base.dy]);
        expect([fy.dx, fy.dy]).toEqual([base.dx, -base.dy]);
    });

    it("inversion is presentation only — aMove's step count is unchanged", () => {
        const flipped = withInvert({ a: !axes.a.invert });
        forEachSize((n) => {
            const t = aMove(n, flipped).reduce((s, x) => s + Math.abs(x.da), 0);
            return t === n ? null : `N=${n}: ${t}`;
        });
    });
});

describe("choreograph INVARIANTS: the no-op cases produce nothing", () => {
    it("aMove(0) is empty", () => {
        expect(aMove(0, axes)).toEqual([]);
    });

    it("aMove rounds toward zero: |da| < 1 emits nothing", () => {
        expect(aMove(0.7, axes)).toEqual([]);
        expect(aMove(-0.7, axes)).toEqual([]);
    });

    it("zStepCount is 0 for a non-positive lift", () => {
        expect(zStepCount(0, axes)).toBe(0);
        expect(zStepCount(-1, axes)).toBe(0);
        expect(zStepCount(2.0, axes)).toBe(2400);
    });

    it("travelJog emits nothing when rounded position does not change", () => {
        expect(travelJog(100, 100, 100, 100, axes, 0.5, 80)).toEqual([]);
        expect(travelJog(100.1, 100.1, 100.3, 100.3, axes, 0.5, 80)).toEqual([]);
    });

    it("aMoveTo returns nothing when already at target", () => {
        const r = aMoveTo(90, Math.round(90 * axes.a.stepsPerUnit), axes);
        expect(r.segments).toEqual([]);
        expect(r.newAPhys).toBe(Math.round(90 * axes.a.stepsPerUnit));
    });

    it("headOffsetJog emits nothing for identical heads", () => {
        const h = head({ xOffset: -50, yOffset: 0 });
        expect(headOffsetJog(h, h, axes, 0.5, 80)).toEqual([]);
    });

    it("headOffsetJog emits nothing when the offset delta rounds below one step", () => {
        const a = head({ xOffset: 0, yOffset: 0 });
        const b = head({ xOffset: 0.001, yOffset: 0.001 });
        expect(headOffsetJog(a, b, axes, 0.5, 80)).toEqual([]);
    });
});

// ─────────────────────────────────────────────────────────────────────────────
// CONTRACT PROPERTIES
// ─────────────────────────────────────────────────────────────────────────────

describe("choreograph CONTRACT: preOrient's two modes", () => {
    it("a non-tangential tool is never pre-oriented", () => {
        const r = preOrient(90, 0, 500, axes, PEN);
        expect(r.segments).toEqual([]);
        expect(r.newAPhys).toBe(500);
    });

    it("unwind (KNIFE) targets an ABSOLUTE angle regardless of where A is", () => {
        const target = Math.round(90 * axes.a.stepsPerUnit);
        for (const from of [0, 4650, -4650, 18600]) {
            expect(preOrient(90, 0, from, axes, KNIFE).newAPhys).toBe(target);
        }
    });

    it("unwind bounds |aPhys| — A cannot wind away over many paths", () => {
        // Whatever the tool did while cutting, the next path re-datums A to
        // the entry tangent. |aPhys| after preOrient is bounded by 180*spu.
        const bound = 180 * axes.a.stepsPerUnit + 1;
        let phys = 0;
        for (const entry of [10, -170, 175, -5, 90, -90, 179, -179]) {
            phys = preOrient(entry, 0, phys, axes, KNIFE).newAPhys;
            expect(Math.abs(phys)).toBeLessThanOrEqual(bound);
            phys += 6000; // simulate a path that winds A hard while cutting
        }
    });

    it("non-unwind (CREASE) rotates by the SHORTEST delta, never the long way", () => {
        // 170 -> -170 is +20 degrees, not -340.
        const r = preOrient(-170, 170, 0, axes, CREASE);
        const steps = Math.abs(r.newAPhys);
        expect(steps).toBeCloseTo(20 * axes.a.stepsPerUnit, 0);
    });

    it("non-unwind never rotates more than 180 degrees", () => {
        for (let cur = -180; cur <= 180; cur += 17) {
            for (let entry = -180; entry <= 180; entry += 23) {
                const r = preOrient(entry, cur, 0, axes, CREASE);
                expect(Math.abs(r.newAPhys)).toBeLessThanOrEqual(180 * axes.a.stepsPerUnit + 1);
            }
        }
    });

    it("non-unwind ignores accumulated aPhys; unwind consumes it", () => {
        const free = preOrient(90, 0, 9999, axes, CREASE);
        const wired = preOrient(90, 0, 9999, axes, KNIFE);
        expect(free.newAPhys - 9999).toBe(preOrient(90, 0, 0, axes, CREASE).newAPhys);
        expect(wired.newAPhys).toBe(Math.round(90 * axes.a.stepsPerUnit));
    });
});

describe("choreograph CONTRACT: pivot ordering", () => {
    it("lift happens before the rotation and lower after it", () => {
        const segs = pivot(1000, true, 2400, axes, 20);
        const zIdx = segs.map((s, i) => (s.dz !== 0 ? i : -1)).filter((i) => i >= 0);
        const aIdx = segs.map((s, i) => (s.da !== 0 ? i : -1)).filter((i) => i >= 0);
        expect(zIdx.length).toBe(2);
        expect(zIdx[0]).toBeLessThan(Math.min(...aIdx));
        expect(zIdx[1]).toBeGreaterThan(Math.max(...aIdx));
    });

    it("no Z motion is emitted when lift is false", () => {
        for (const s of pivot(1000, false, 0, axes, 20)) expect(s.dz).toBe(0);
    });

    it("pivot's A motion is exactly aMove's", () => {
        const p = pivot(1000, true, 2400, axes, 20).filter((s) => s.da !== 0);
        expect(p).toEqual(aMove(1000, axes));
    });

    it("a zero-rotation pivot with lift still lifts and lowers (and nothing else)", () => {
        const segs = pivot(0, true, 2400, axes, 20);
        expect(segs.length).toBe(2);
        expect(segs[0]!.dz).toBe(-segs[1]!.dz);
    });
});

describe("choreograph CONTRACT: travelJog and headOffsetJog geometry", () => {
    it("travelJog moves the rounded step delta, both signs", () => {
        for (const [fx, fy, tx, ty] of [
            [0, 0, 3200, 1600], [3200, 1600, 0, 0], [-500, 250, 500, -250],
        ]) {
            const m = sum(travelJog(fx!, fy!, tx!, ty!, axes, 0.5, 80));
            const dx = Math.round(tx!) - Math.round(fx!);
            const dy = Math.round(ty!) - Math.round(fy!);
            expect(m.dx).toBe(axes.x.invert ? -dx : dx);
            expect(m.dy).toBe(axes.y.invert ? -dy : dy);
        }
    });

    it("travelJog rounds each endpoint rather than truncating", () => {
        // trunc would lose a step whenever the two endpoints straddle .5 the
        // same way, and the loss would accumulate across a whole job.
        const m = sum(travelJog(0.6, 0.6, 10.6, 10.6, axes, 0.5, 80));
        expect(Math.abs(m.dx)).toBe(10); // round: 11-1; trunc would give 10-0
        const n = sum(travelJog(0.4, 0.4, 10.6, 10.6, axes, 0.5, 80));
        expect(Math.abs(n.dx)).toBe(11); // round: 11-0; trunc would give 10-0
    });

    it("travelJog is antisymmetric: there and back cancels", () => {
        const there = sum(travelJog(0, 0, 3200, 1600, axes, 0.5, 80));
        const back = sum(travelJog(3200, 1600, 0, 0, axes, 0.5, 80));
        expect(there.dx + back.dx).toBe(0);
        expect(there.dy + back.dy).toBe(0);
    });

    it("headOffsetJog moves by (to - from), so the new head lands where the old was", () => {
        const from = head({ xOffset: -50, yOffset: 0 });
        const to = head({ xOffset: 50, yOffset: 10 });
        const m = sum(headOffsetJog(from, to, axes, 0.5, 80));
        const dx = Math.round(100 * axes.x.stepsPerUnit);
        const dy = Math.round(10 * axes.y.stepsPerUnit);
        expect(m.dx).toBe(axes.x.invert ? -dx : dx);
        expect(m.dy).toBe(axes.y.invert ? -dy : dy);
    });

    it("headOffsetJog is antisymmetric", () => {
        const a = head({ xOffset: -50, yOffset: 3 });
        const b = head({ xOffset: 50, yOffset: 10 });
        const there = sum(headOffsetJog(a, b, axes, 0.5, 80));
        const back = sum(headOffsetJog(b, a, axes, 0.5, 80));
        expect(there.dx + back.dx).toBe(0);
        expect(there.dy + back.dy).toBe(0);
    });
});

describe("choreograph CONTRACT: emitted timing matches the requested feed", () => {
    it("zMove takes liftHeight / zFeed seconds", () => {
        for (const [mm, feed] of [[2, 20], [5, 20], [2, 10]]) {
            const steps = zStepCount(mm!, axes);
            const m = zMove(steps, axes, feed!);
            const seconds = (steps * m.interval) / axes.fCpu;
            expect(seconds).toBeCloseTo(mm! / feed!, 3);
        }
    });

    it("travelJog takes distance / jogFeed seconds, plus its ramps", () => {
        // A ramped jog cannot be FASTER than the constant-feed ideal, and the
        // ramp overhead is a fixed time cost, so it shrinks as a fraction of a
        // longer move. Both halves matter: the first says the feed is still
        // respected as a ceiling, the second says ramping did not quietly
        // double the duration of ordinary travel.
        for (const [mm, feed, tol] of [[200, 80, 0.05], [50, 80, 0.2], [200, 40, 0.03]]) {
            const steps = mm! * axes.x.stepsPerUnit;
            const sl = slices(travelJog(0, 0, steps, 0, axes, 0.5, feed!));
            const seconds = sl.reduce((t, x) => t + x.dt, 0);
            const ideal = mm! / feed!;
            expect(seconds).toBeGreaterThanOrEqual(ideal * 0.999);
            expect(seconds / ideal - 1).toBeLessThan(tol!);
        }
    });

    it("zStepCount rounds rather than truncates", () => {
        expect(zStepCount(1.7005, axes)).toBe(Math.round(1.7005 * 1200)); // 2041, not 2040
    });

    it("zMove's interval stays in range at absurd feeds", () => {
        expect(zMove(100, axes, 1e9).interval).toBe(1);              // would trunc to 0
        expect(zMove(100, axes, 1e-9).interval).toBe(axes.fCpu);     // would exceed fCpu
    });

    it("a long aMove actually reaches the A feed ceiling", () => {
        // Not just "stays under" — the cruise phase must be the ceiling, or the
        // axis is being driven far below what the machine can do.
        const peak = Math.max(...slices(aMove(18600, axes)).map((s) => s.v));
        expect(peak).toBeGreaterThan(A_CRUISE * 0.99);
    });

    it("aMove's total time is not far ABOVE the analytic trapezoid either", () => {
        // Pairs with the lower bound below: together they pin the ramp to the
        // real limits, so a ramp built from the wrong units cannot pass.
        const v0 = Math.min(A_CRUISE, 50);
        forEachSize((n) => {
            const t = slices(aMove(n, axes)).reduce((s, x) => s + x.dt, 0);
            let dAcc = (A_CRUISE * A_CRUISE - v0 * v0) / (2 * A_ACCEL);
            const ideal = 2 * dAcc > n
                ? (2 * (Math.sqrt(v0 * v0 + A_ACCEL * n) - v0)) / A_ACCEL
                : (2 * (A_CRUISE - v0)) / A_ACCEL + (n - 2 * dAcc) / A_CRUISE;
            // 1.35 accommodates the coarsest case (N=52 runs in 5 chunks, at
            // 1.32x): chunk quantisation genuinely costs time on tiny moves.
            // A ramp built from the wrong units lands far outside this.
            return t <= ideal * 1.35 ? null : `N=${n}: ${t.toFixed(4)}s vs ideal ${ideal.toFixed(4)}s`;
        });
    });

    it("aMove's peak respects the triangular clamp on short moves", () => {
        // A move too short to reach cruise may only accelerate for half its
        // length, or it cannot stop in the other half.
        const v0 = Math.min(A_CRUISE, 50);
        forEachSize((n) => {
            const peak = Math.max(...slices(aMove(n, axes)).map((s) => s.v));
            const reachable = Math.sqrt(v0 * v0 + 2 * A_ACCEL * (n / 2));
            return peak <= Math.min(A_CRUISE, reachable) * 1.02
                ? null
                : `N=${n}: peak ${peak.toFixed(0)} exceeds half-length reachable ${reachable.toFixed(0)}`;
        });
    });

    it("aMove decelerates at all — it does not end at its peak rate", () => {
        // Companion to H1b, which is red and would otherwise mask the loss of
        // the decel branch entirely. This asks only whether a ramp-down exists,
        // not whether it is steep enough. It used to be scoped to n >= 129,
        // because the shortest rotation did not ramp down at all (H1c); that
        // exemption is gone.
        forEachSize((n) => {
            const sl = slices(aMove(n, axes));
            const peak = Math.max(...sl.map((x) => x.v));
            const vEnd = sl[sl.length - 1]!.v;
            return vEnd < peak * 0.95 ? null : `N=${n}: ends at ${vEnd.toFixed(0)} of peak ${peak.toFixed(0)}`;
        });
    });

    it("aMove's segment count is bounded by the ramp, not by the steps", () => {
        // One segment per step would be correct motion and ruinous bandwidth:
        // a 360 deg turn is 18600 steps but must not be 18600 wire segments.
        //
        // The bound is now FLAT — two ramps of RAMP_CHUNKS pieces plus one
        // cruise piece — where it used to grow with the move (the old chunk
        // size was trunc(v/100) steps, so 18600 steps meant 371 segments). A
        // full turn now costs 33. Cutting the wire cost of the largest moves by
        // 11x while making them accel-correct was not a trade; it fell out of
        // choosing chunk boundaries from the speed profile instead of the speed.
        forEachSize((n) => {
            const count = aMove(n, axes).length;
            return count <= 33 ? null : `N=${n}: ${count} segments (>33)`;
        });
    });

    it("aMove never commands the A axis above its feed ceiling", () => {
        forEachSize((n) => {
            const peak = Math.max(...slices(aMove(n, axes)).map((s) => s.v));
            return peak <= A_CRUISE * 1.001
                ? null
                : `N=${n}: peak ${peak.toFixed(0)} > ceiling ${A_CRUISE.toFixed(0)}`;
        });
    });

    it("aMove's total time is at least the analytic trapezoid time", () => {
        // Quantisation may only ever make the move SLOWER than the ideal ramp.
        const v0 = Math.min(A_CRUISE, 50);
        forEachSize((n) => {
            const t = slices(aMove(n, axes)).reduce((s, x) => s + x.dt, 0);
            let dAcc = (A_CRUISE * A_CRUISE - v0 * v0) / (2 * A_ACCEL);
            const ideal = 2 * dAcc > n
                ? (2 * (Math.sqrt(v0 * v0 + A_ACCEL * n) - v0)) / A_ACCEL
                : (2 * (A_CRUISE - v0)) / A_ACCEL + (n - 2 * dAcc) / A_CRUISE;
            return t >= ideal * 0.999 ? null : `N=${n}: ${t.toFixed(4)}s < ideal ${ideal.toFixed(4)}s`;
        });
    });
});

// ── the red ones: each names the finding it pins ─────────────────────────────

describe("choreograph CONTRACT: acceleration limits (FINDINGS)", () => {
    it("H1a (FIXED): both of aMove's ramps respect the A accel ceiling", () => {
        // Was: the accel ramp was fine (<=0.88x) and the decel ramp overshot on
        // every size, because `v` was sampled at each chunk's START — the
        // SLOWEST point of an accelerating chunk (conservative) and the FASTEST
        // of a decelerating one (anti-conservative). One line, opposite sign on
        // the two halves of the same move.
        //
        // Now each chunk's interval comes from the exact constant-accel time
        // across it, so the demand is the accel limit itself at every boundary.
        // The bound is 1.0 and the measurement lands ON it, not under it: that
        // is the design — the ramp is meant to use the whole ceiling. A slack
        // bound here would stop pinning anything.
        forEachSize((n) => {
            const { up, down } = rampRatios(slices(aMove(n, axes)), A_ACCEL);
            const worst = Math.max(up, down);
            return worst <= 1.001 ? null : `N=${n}: demands ${worst.toFixed(2)}x the A accel limit`;
        });
    });

    it("H1b (FIXED): aMove can come to rest within its final chunk", () => {
        // Was: the move stopped dead from 8.85-39.36 deg/s, having never
        // reached its designed terminal velocity of 0.97 deg/s.
        //
        // Terminal velocity is not directly readable from the stream — the last
        // chunk's rate is its MEAN, and the profile's true end speed is v0. So
        // the property to assert is the one that matters physically: whatever
        // rate the final chunk commands, the axis must be able to reach zero
        // from it within that chunk's own duration.
        forEachSize((n) => {
            const sl = slices(aMove(n, axes));
            const last = sl[sl.length - 1]!;
            const demand = last.v / last.dt / A_ACCEL;
            return demand <= 1.0
                ? null
                : `N=${n}: stopping from ${(last.v / axes.a.stepsPerUnit).toFixed(2)} deg/s` +
                  ` in ${(last.dt * 1000).toFixed(2)}ms demands ${demand.toFixed(2)}x the limit`;
        });
    });

    it("H1c (FIXED): the shortest rotations ramp down as well as up", () => {
        // H1's cause at its most vivid. A 1 degree pivot (N=52) used to run in
        // five chunks whose speeds only ever went UP (50 -> 457 -> 1018 ->
        // 1761 -> 2034), ending at its own peak: the "decel" chunk was faster
        // than the cruise chunk before it, because the decel rate was read at
        // the chunk's start where the remaining distance is greatest.
        const sl = slices(aMove(52, axes));
        const peak = Math.max(...sl.map((x) => x.v));
        const iPeak = sl.findIndex((x) => x.v === peak);
        expect(sl[sl.length - 1]!.v).toBeLessThan(peak);
        expect(iPeak).toBeLessThan(sl.length - 1);        // peaks before the end
        expect(iPeak).toBeGreaterThan(0);                  // and after the start
        // and the profile is symmetric: it comes back down to where it started
        expect(sl[sl.length - 1]!.v).toBeCloseTo(sl[0]!.v, 0);
    });

    it("H2 (FIXED): travelJog ramps to its feed instead of stepping straight to it", () => {
        // Was: one segment at full jogFeed — 0 -> 80 mm/s in zero distance,
        // against a configured x.maxAccel of 1000 mm/s^2 that needed 3.2mm of
        // ramp. Now the jog is a trapezoid like any other move, so it must
        // open slow and never demand more than the axis has.
        for (const mm of [5, 20, 200]) {
            const sl = slices(travelJog(0, 0, mm * axes.x.stepsPerUnit, 0, axes, 0.5, 80));
            expect(sl.length).toBeGreaterThan(1);
            const limit = axes.x.maxAccel * axes.x.stepsPerUnit; // steps/s^2
            // It must open well below feed — but not at v0 itself: the first
            // chunk's rate is its mean, and one step at this accel already
            // carries the axis well past its junction speed.
            const openMmS = sl[0]!.v / axes.x.stepsPerUnit;
            expect(openMmS).toBeLessThan(80 / 4);
            expect(sl[sl.length - 1]!.v).toBeCloseTo(sl[0]!.v, 0); // symmetric
            expect(worstAccelRatio(sl, limit)).toBeLessThanOrEqual(1.001);
        }
    });

    it("H2 (FIXED): the tighter of the two XY axes owns the jog's ramp", () => {
        // The fixture gives x and y the same maxAccel, so a jog cannot tell
        // min from max there. Skew them: a diagonal move must ramp against the
        // WEAKER axis, and must not get faster when only the stronger one is
        // raised.
        const weakY = remap({ y: { maxAccel: 100 } });
        const sl = slices(travelJog(0, 0, 16000, 16000, weakY, 0.5, 80), weakY);
        const secs = sl.reduce((t, x) => t + x.dt, 0);
        // measured against the weak axis's own ceiling, the ramp is legal
        expect(worstAccelRatio(sl, 100 * weakY.y.stepsPerUnit)).toBeLessThanOrEqual(1.001);
        // and it is genuinely slower than the same jog on the stiff machine
        const stiff = slices(travelJog(0, 0, 16000, 16000, axes, 0.5, 80));
        expect(secs).toBeGreaterThan(stiff.reduce((t, x) => t + x.dt, 0));
        // raising only the stronger axis must change nothing
        const strongX = remap({ x: { maxAccel: 100000 }, y: { maxAccel: 100 } });
        const alt = slices(travelJog(0, 0, 16000, 16000, strongX, 0.5, 80), strongX);
        expect(alt.reduce((t, x) => t + x.dt, 0)).toBeCloseTo(secs, 6);
    });

    it("H2 (FIXED): a ramped jog still travels in a straight line", () => {
        // Ramping splits one segment into ~33, so the two axes are now stepped
        // in pieces and could stair-step off the diagonal. Each chunk's
        // cumulative position must stay on the ideal line to within a step.
        const dx = 16000;
        const dy = 7000;
        const segs = travelJog(0, 0, dx, dy, axes, 0.5, 80);
        let cx = 0;
        let cy = 0;
        let worst = 0;
        for (const s of segs) {
            cx += axes.x.invert ? -s.dx : s.dx;
            cy += axes.y.invert ? -s.dy : s.dy;
            worst = Math.max(worst, Math.abs(cy - (cx * dy) / dx));
        }
        expect(worst).toBeLessThanOrEqual(1);
        expect([cx, cy]).toEqual([dx, dy]); // and it lands exactly
    });

    it("H2 (FIXED): headOffsetJog ramps too — it is the same emitter", () => {
        const sl = slices(headOffsetJog(
            head({ xOffset: -50, yOffset: 0 }), head({ xOffset: 50, yOffset: 10 }),
            axes, 0.5, 80,
        ));
        expect(sl.length).toBeGreaterThan(1);
        expect(worstAccelRatio(sl, axes.x.maxAccel * axes.x.stepsPerUnit)).toBeLessThanOrEqual(1.001);
    });

    it("H3: zMove ramps instead of slamming to zFeed", () => {
        // Acknowledged by the TODO at the top of choreograph.ts. Recorded as a
        // test so it is counted, not just commented. z.maxAccel is 0
        // (uncharacterized), so there is no ceiling to measure against yet.
        const m = zMove(zStepCount(2, axes), axes, 20);
        const v = axes.fCpu / m.interval;
        expect(
            `one segment opening at ${v.toFixed(0)} steps/s,` +
            ` z.maxAccel=${axes.z.maxAccel} (uncharacterized)`,
        ).toBe("a ramped Z move against a known z.maxAccel");
    });
});

describe("choreograph CONTRACT: fallbacks", () => {
    it("H4: aMove refuses to invent A limits for an under-specified machine", () => {
        // load.ts refuses to invent stepsPerUnit/invert/node because guessing
        // calibration is how you crash a machine. aMove used to invent
        // 180 deg/s and 2000 deg/s^2 when the A ceilings were 0 ("uncapped"),
        // silently — which made an undeclared axis run 1.8x FASTER than the
        // real machine's declared 100 deg/s. Same class of number, and now the
        // same policy.
        expect(() => aMove(4650, remap({ a: { maxFeed: 0, maxAccel: 0 } })))
            .toThrow(/no feed and accel limit/);
        expect(() => aMove(4650, remap({ a: { maxFeed: 0 } })))
            .toThrow(/no feed limit/);
        expect(() => aMove(4650, remap({ a: { maxAccel: 0 } })))
            .toThrow(/no accel limit/);
    });

    it("H4: an explicit slew target satisfies an otherwise uncapped A axis", () => {
        // "Uncapped" is refused for lack of a number, not as a policy against
        // the axis — supplying the number by any route is enough.
        const uncapped = remap({ a: { maxFeed: 0, maxAccel: 0 } });
        const segs = aMove(4650, uncapped, { feed: 100, accel: 500 });
        expect(segs.reduce((s, x) => s + Math.abs(x.da), 0)).toBe(4650);
    });

    it("H4: a zero rotation on an uncapped axis is still a no-op, not a throw", () => {
        // Nothing to rotate needs no limits. Keeps an absent A axis (which
        // load.ts builds with 0 ceilings) from throwing on a no-op call.
        expect(aMove(0, remap({ a: { maxFeed: 0, maxAccel: 0 } }))).toEqual([]);
    });
});
