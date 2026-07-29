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
 * At each slice boundary the commanded rate changes instantly. A machine
 * limited to `limit` can only follow that if the change fits inside the
 * PRECEDING slice's duration — so the demand is |dv| / dt_prev. This asks a
 * question about the emitted bytes alone; it never consults the emitter.
 */
function worstAccelRatio(sl: readonly Slice[], limit: number): number {
    let worst = 0;
    for (let i = 1; i < sl.length; i++) {
        worst = Math.max(worst, Math.abs(sl[i]!.v - sl[i - 1]!.v) / sl[i - 1]!.dt / limit);
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
            travelJog(0, 0, 32000, 16000, axes, 0.5, 80)!,
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
            travelJog(0.4, 0.6, 321.7, 89.2, axes, 0.5, 80)!,
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
        const m = travelJog(0, 0, 3200, 1600, axes, 0.5, 80)!;
        expect(m.flags).toBe(MICRO_JOG);
        expect([m.dz, m.da]).toEqual([0, 0]);
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
        const base = travelJog(0, 0, 3200, 1600, axes, 0.5, 80)!;
        const fx = travelJog(0, 0, 3200, 1600, withInvert({ x: !axes.x.invert }), 0.5, 80)!;
        const fy = travelJog(0, 0, 3200, 1600, withInvert({ y: !axes.y.invert }), 0.5, 80)!;
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

    it("travelJog returns null when rounded position does not change", () => {
        expect(travelJog(100, 100, 100, 100, axes, 0.5, 80)).toBeNull();
        expect(travelJog(100.1, 100.1, 100.3, 100.3, axes, 0.5, 80)).toBeNull();
    });

    it("aMoveTo returns nothing when already at target", () => {
        const r = aMoveTo(90, Math.round(90 * axes.a.stepsPerUnit), axes);
        expect(r.segments).toEqual([]);
        expect(r.newAPhys).toBe(Math.round(90 * axes.a.stepsPerUnit));
    });

    it("headOffsetJog returns null for identical heads", () => {
        const h = head({ xOffset: -50, yOffset: 0 });
        expect(headOffsetJog(h, h, axes, 0.5, 80)).toBeNull();
    });

    it("headOffsetJog returns null when the offset delta rounds below one step", () => {
        const a = head({ xOffset: 0, yOffset: 0 });
        const b = head({ xOffset: 0.001, yOffset: 0.001 });
        expect(headOffsetJog(a, b, axes, 0.5, 80)).toBeNull();
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
            const m = travelJog(fx!, fy!, tx!, ty!, axes, 0.5, 80)!;
            const dx = Math.round(tx!) - Math.round(fx!);
            const dy = Math.round(ty!) - Math.round(fy!);
            expect(m.dx).toBe(axes.x.invert ? -dx : dx);
            expect(m.dy).toBe(axes.y.invert ? -dy : dy);
        }
    });

    it("travelJog rounds each endpoint rather than truncating", () => {
        // trunc would lose a step whenever the two endpoints straddle .5 the
        // same way, and the loss would accumulate across a whole job.
        const m = travelJog(0.6, 0.6, 10.6, 10.6, axes, 0.5, 80)!;
        expect(Math.abs(m.dx)).toBe(10); // round: 11-1; trunc would give 10-0
        const n = travelJog(0.4, 0.4, 10.6, 10.6, axes, 0.5, 80)!;
        expect(Math.abs(n.dx)).toBe(11); // round: 11-0; trunc would give 10-0
    });

    it("travelJog is antisymmetric: there and back cancels", () => {
        const there = travelJog(0, 0, 3200, 1600, axes, 0.5, 80)!;
        const back = travelJog(3200, 1600, 0, 0, axes, 0.5, 80)!;
        expect(there.dx + back.dx).toBe(0);
        expect(there.dy + back.dy).toBe(0);
    });

    it("headOffsetJog moves by (to - from), so the new head lands where the old was", () => {
        const from = head({ xOffset: -50, yOffset: 0 });
        const to = head({ xOffset: 50, yOffset: 10 });
        const m = headOffsetJog(from, to, axes, 0.5, 80)!;
        const dx = Math.round(100 * axes.x.stepsPerUnit);
        const dy = Math.round(10 * axes.y.stepsPerUnit);
        expect(m.dx).toBe(axes.x.invert ? -dx : dx);
        expect(m.dy).toBe(axes.y.invert ? -dy : dy);
    });

    it("headOffsetJog is antisymmetric", () => {
        const a = head({ xOffset: -50, yOffset: 3 });
        const b = head({ xOffset: 50, yOffset: 10 });
        const there = headOffsetJog(a, b, axes, 0.5, 80)!;
        const back = headOffsetJog(b, a, axes, 0.5, 80)!;
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

    it("travelJog takes distance / jogFeed seconds", () => {
        for (const [mm, feed] of [[200, 80], [50, 80], [200, 40]]) {
            const steps = mm! * axes.x.stepsPerUnit;
            const m = travelJog(0, 0, steps, 0, axes, 0.5, feed!)!;
            const seconds = (Math.abs(m.dx) * m.interval) / axes.fCpu;
            expect(seconds).toBeCloseTo(mm! / feed!, 2);
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
        // not whether it is steep enough. Scoped above the shortest rotation,
        // which does not ramp down at all — that case is H1c.
        forEachSize((n) => {
            if (n < 129) return null;
            const sl = slices(aMove(n, axes));
            const peak = Math.max(...sl.map((x) => x.v));
            const vEnd = sl[sl.length - 1]!.v;
            return vEnd < peak * 0.95 ? null : `N=${n}: ends at ${vEnd.toFixed(0)} of peak ${peak.toFixed(0)}`;
        });
    });

    it("aMove's segment count stays proportional to the ramp, not to the steps", () => {
        // One segment per step would be correct motion and ruinous bandwidth:
        // a 360 deg turn is 18600 steps but must not be 18600 wire segments.
        forEachSize((n) => {
            const count = aMove(n, axes).length;
            return count <= Math.max(16, n / 20)
                ? null
                : `N=${n}: ${count} segments (>${Math.max(16, Math.floor(n / 20))})`;
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
    it("H1a: aMove's decel ramp respects the A accel ceiling", () => {
        // The accel ramp is fine (<=0.88x). The decel ramp overshoots on every
        // size, because `v` is sampled at each chunk's START: on the way up
        // that is the SLOWEST point in the chunk (conservative), on the way
        // down it is the FASTEST (anti-conservative). Same line, opposite sign.
        forEachSize((n) => {
            const { down } = rampRatios(slices(aMove(n, axes)), A_ACCEL);
            return down <= 1.02 ? null : `N=${n}: decel demands ${down.toFixed(2)}x the A accel limit`;
        });
    });

    it("H1b: aMove comes to rest at its designed terminal velocity", () => {
        // aMove ramps down toward v0 = min(cruise, 50) = 50 steps/s (0.97 deg/s)
        // and then simply stops. The last slice's rate is what the A axis is
        // actually doing when the move ends.
        const v0 = Math.min(A_CRUISE, 50);
        forEachSize((n) => {
            const sl = slices(aMove(n, axes));
            const vEnd = sl[sl.length - 1]!.v;
            return vEnd <= v0 * 1.02
                ? null
                : `N=${n}: stops from ${(vEnd / axes.a.stepsPerUnit).toFixed(2)} deg/s,` +
                  ` designed ${(v0 / axes.a.stepsPerUnit).toFixed(2)} deg/s`;
        });
    });

    it("H1c: the shortest rotations ramp down at all", () => {
        // H1's cause at its most vivid. A 1 degree pivot (N=52) runs in five
        // chunks and its single "decel" chunk is FASTER than the cruise chunk
        // before it — because the decel rate is read at the chunk's start,
        // where the remaining distance, and so the speed, is greatest. The
        // rotation accelerates into its final chunk and then simply stops.
        const sl = slices(aMove(52, axes));
        const peak = Math.max(...sl.map((x) => x.v));
        const vEnd = sl[sl.length - 1]!.v;
        expect(
            `N=52 ends at ${vEnd.toFixed(0)} steps/s, its own peak` +
            ` (${sl.map((s) => s.v.toFixed(0)).join(" -> ")})`,
        ).toBe(`N=52 ends below its peak of ${peak.toFixed(0)} steps/s`);
    });

    it("H2: travelJog ramps to its feed instead of stepping straight to it", () => {
        // A travel jog is one segment at full jogFeed. The X axis has a real,
        // configured accel ceiling (x.maxAccel) that this move ignores
        // entirely: it goes 0 -> jogFeed in zero distance.
        const mm = 200;
        const m = travelJog(0, 0, mm * axes.x.stepsPerUnit, 0, axes, 0.5, 80)!;
        const vStepsPerS = axes.fCpu / m.interval;
        const vMmPerS = vStepsPerS / axes.x.stepsPerUnit;
        const rampMm = (vMmPerS * vMmPerS) / (2 * axes.x.maxAccel);
        expect(
            `${mm}mm jog opens at ${vMmPerS.toFixed(1)} mm/s in one segment;` +
            ` reaching that at x.maxAccel=${axes.x.maxAccel} needs ${rampMm.toFixed(2)}mm`,
        ).toBe(`ramped over >= ${rampMm.toFixed(2)}mm`);
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

describe("choreograph CONTRACT: fallbacks (FINDING)", () => {
    it("H4: aMove does not invent A limits for an under-specified machine", () => {
        // load.ts refuses to invent stepsPerUnit/invert/node because guessing
        // calibration is how you crash a machine. aMove happily invents
        // 180 deg/s and 2000 deg/s^2 when the A ceilings are 0 ("uncapped"),
        // silently, at the emitter. Same class of number, opposite policy.
        const uncapped = remap({ a: { maxFeed: 0, maxAccel: 0 } });
        const peak = Math.max(...slices(aMove(4650, uncapped), uncapped).map((s) => s.v));
        const invented = 180 * uncapped.a.stepsPerUnit;
        expect(
            `uncapped A move peaks at ${peak.toFixed(0)} steps/s` +
            ` (= the invented ${invented.toFixed(0)} floor)`,
        ).toBe("no motion emitted, or an explicit error");
    });
});
