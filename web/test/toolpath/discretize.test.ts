/**
 * Tests for the Discretize stage (redesign stage 8): planned Sample stream ->
 * MicroSegment[]. The bottom of the pipeline — what comes out of here goes on
 * the wire, so a defect here is a defect on metal.
 *
 * Split, as with flatten / constrain / plan (docs/planner_audit.md):
 *
 *   INVARIANTS          — must hold for every input. A failure is a bug.
 *   CONTRACT PROPERTIES — what the stage's doc comment claims. Some FAIL; each
 *                         failing test names the finding it pins.
 *
 * Method note carried from the earlier stages: measure independently of the
 * implementation. `emittedSeconds` re-derives wall time from interval and step
 * counts the way the FIRMWARE will, not the way discretize computed it — which
 * is how D2 (interval's rate floor is a second, unmodelled speed governor) came
 * to light. Fixture loops aggregate via forEachFixture and fail once, so the
 * worst case cannot hide behind the first.
 */

import { describe, it, expect } from "vitest";
import { readFixture } from "../helpers.js";
import { lineToCubic, angleDelta, type CubicBezier } from "../../src/toolpath/geometry.js";
import { flatten } from "../../src/toolpath/flatten.js";
import { constrain } from "../../src/toolpath/constrain.js";
import { plan, subpathRanges, type PlannedSample } from "../../src/toolpath/plan.js";
import { discretize } from "../../src/toolpath/discretize.js";
import {
    MICRO_PATH_END,
    MICRO_JOG,
    MICRO_LIFT,
    type MicroSegment,
} from "../../src/wire/format/microsegment.js";
import { enforceC1 } from "../../src/toolpath/repair.js";
import { CASES, CUSP } from "./curves.cases.js";
import { resolvedAxesDefault, qualityConfig, KNIFE, PEN } from "../../src/machine/index.js";
import { defaultConfig } from "../machines.js";
import { loadSvgMmSubpaths } from "../../src/svg/ingest.js";

const CFG = defaultConfig();
const MACH = CFG.machine;
const AXES = resolvedAxesDefault(MACH);
const HEAD = MACH.heads[MACH.defaultHead]!;
const q = qualityConfig();
const FEED = 80.0;
const A_MAX = 1000.0;

type Profile = typeof KNIFE | typeof PEN;

const GEOMETRY_CASES: Record<string, readonly CubicBezier[]> = Object.fromEntries([
    ...Object.entries(CASES).map(([k, v]) => [k, v.curves]),
    ["cusp", CUSP],
]);

function line(p0: { x: number; y: number }, p1: { x: number; y: number }): CubicBezier {
    return lineToCubic(p0, p1);
}

function planFor(
    subpaths: readonly (readonly CubicBezier[])[],
    profile: Profile,
): PlannedSample[] {
    const s = flatten(subpaths, q);
    const c = constrain(s, {
        feedMax: FEED,
        aMax: A_MAX,
        junctionDeviation: q.junctionDeviation,
        ...(profile.tangential
            ? {
                  aRateDegS: AXES.a.maxFeed,
                  aAccelDegS2: HEAD.a.maxAccel,
                  cornerStopAngleDeg: profile.cornerAngleDeg,
              }
            : {}),
        // the production bridge passes this; the tests must too, or they
        // measure a pipeline nobody ships (audit C1)
        vMin: q.vMin,
    });
    return plan(c, {
        xAccel: MACH.x.maxAccel,
        yAccel: MACH.y.maxAccel,
        aAccelDegS2: HEAD.a.maxAccel,
        aMax: A_MAX,
    });
}

function prep(
    subpaths: readonly (readonly CubicBezier[])[],
    profile: Profile,
): MicroSegment[] {
    return discretize(planFor(subpaths, profile), MACH, profile, q);
}

function forEachFixture(
    probe: (name: string, curves: readonly CubicBezier[]) => string[],
): void {
    const violations: string[] = [];
    for (const [name, curves] of Object.entries(GEOMETRY_CASES)) {
        violations.push(...probe(name, curves));
    }
    if (violations.length > 0) {
        throw new Error(`${violations.length} violation(s):\n  ${violations.join("\n  ")}`);
    }
}

// ── measurement helpers (deliberately firmware-shaped, not discretize-shaped) ──

/** Steps the firmware will clock on this segment: the major axis. */
function major(s: MicroSegment): number {
    return Math.max(Math.abs(s.dx), Math.abs(s.dy), Math.abs(s.dz), Math.abs(s.da));
}

/**
 * Wall time the FIRMWARE will spend on these segments: interval cycles per
 * major-axis step, times steps, over the clock. Derived the way the executor
 * derives it — not from the planned v that produced it. That independence is
 * what makes it able to catch D2.
 */
function emittedSeconds(segs: readonly MicroSegment[]): number {
    return segs.reduce((t, s) => t + (s.interval * major(s)) / MACH.fCpu, 0);
}

/** Time the PLAN says the cut takes, with the same vMin floor interval applies. */
function plannedSeconds(p: readonly PlannedSample[]): number {
    let t = 0;
    for (const [lo, hi] of subpathRanges(p)) {
        for (let i = lo; i < hi; i++) {
            t += p[i]!.ds / Math.max(0.5 * (p[i]!.v + p[i + 1]!.v), q.vMin);
        }
    }
    return t;
}

/** Non-cutting motion: travel jogs, Z lifts, pivots, pre-orientation. */
function isChoreography(s: MicroSegment): boolean {
    return (s.flags & MICRO_JOG) !== 0 || (s.flags & MICRO_LIFT) !== 0 || s.dz !== 0;
}

function cutting(segs: readonly MicroSegment[]): MicroSegment[] {
    return segs.filter((s) => !isChoreography(s));
}

function net(segs: readonly MicroSegment[]): [number, number, number] {
    return segs.reduce<[number, number, number]>(
        (a, s) => [a[0] + s.dx, a[1] + s.dy, a[2] + s.da],
        [0, 0, 0],
    );
}

function expectedXY(subpaths: readonly (readonly CubicBezier[])[]): [number, number] {
    const s = flatten(subpaths, q);
    let dx = Math.round(s[s.length - 1]!.x * MACH.x.stepsPerUnit) - Math.round(s[0]!.x * MACH.x.stepsPerUnit);
    let dy = Math.round(s[s.length - 1]!.y * MACH.y.stepsPerUnit) - Math.round(s[0]!.y * MACH.y.stepsPerUnit);
    if (MACH.x.invert) dx = -dx;
    if (MACH.y.invert) dy = -dy;
    return [dx, dy];
}

/**
 * Re-derive discretize's corner rule from the planned stream, returning the
 * index of the sample the PIVOT HAPPENS AT.
 *
 * discretize walks pairs (i, i+1) and pivots after arriving at i+1, so the
 * sample that must be at rest is the LATER one. This used to return `i` while
 * the caller printed the flags of `i + 1`, which under-reported D4: at the cusp
 * the approach sample read 4.84e-3 mm/s and the sample that actually pivots
 * read 2.14e-2 — 4.4x worse than the number the finding was filed with.
 */
function cornerIndices(p: readonly PlannedSample[], profile: Profile): number[] {
    if (!profile.tangential) return [];
    const out: number[] = [];
    for (const [lo, hi] of subpathRanges(p)) {
        let theta = p[lo]!.theta;
        for (let i = lo; i < hi; i++) {
            if (Math.abs(angleDelta(theta, p[i + 1]!.theta)) >= profile.cornerAngleDeg) out.push(i + 1);
            theta = p[i + 1]!.theta;
        }
    }
    return out;
}

// ══════════════════════════════════════════════════════════════════════════════
// INVARIANTS
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 8 INVARIANT: purity and determinism", () => {
    it("does not mutate its input", () => {
        const p = planFor([CASES.s_curve!.curves], KNIFE);
        const before = JSON.stringify(p);
        discretize(p, MACH, KNIFE, q);
        expect(JSON.stringify(p)).toBe(before);
    });

    it("is deterministic", () => {
        const p = planFor([CASES.full_circle_r30!.curves], KNIFE);
        expect(discretize(p, MACH, KNIFE, q)).toEqual(discretize(p, MACH, KNIFE, q));
    });
});

describe("stage 8 INVARIANT: XY conservation", () => {
    // The decisive property: whatever the segment density, the float
    // accumulators telescope to round(last) - round(first), with invert applied.
    it("net XY lands on the geometric endpoint, every fixture, both tools", () => {
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const profile of [KNIFE, PEN]) {
                const [nx, ny] = net(prep([curves], profile));
                const [ex, ey] = expectedXY([curves]);
                if (nx !== ex || ny !== ey) {
                    bad.push(`${name}/${profile.name}: got (${nx},${ny}) want (${ex},${ey})`);
                }
            }
            return bad;
        });
    });

    it("holds across multiple subpaths, including the travel jogs between them", () => {
        const subpaths = [
            CASES.straight_line!.curves,
            CASES.quarter_circle_r5!.curves,
            [line({ x: 200, y: 40 }, { x: 260, y: 90 })],
        ];
        const [nx, ny] = net(prep(subpaths, KNIFE));
        const [ex, ey] = expectedXY(subpaths);
        expect([nx, ny]).toEqual([ex, ey]);
    });

    it("holds on a real SVG through the full repair chain", () => {
        const { subpaths } = loadSvgMmSubpaths(readFixture("test_snake.svg"));
        const repaired = subpaths.map(
            (sp) => enforceC1(sp, { angleTolDeg: q.angleTol, gapTolMm: q.gapTol }).repaired,
        );
        const [nx, ny] = net(prep(repaired, KNIFE));
        expect([nx, ny]).toEqual(expectedXY(repaired));
    });
});

describe("stage 8 INVARIANT: A conservation for a tangential tool", () => {
    it("net A equals the entry orientation plus the total tracked turn", () => {
        // Tracking, pre-orientation and corner pivots must telescope to exactly
        // the geometry's total turn. A drift here is a knife pointing the wrong
        // way, which no downstream stage can detect.
        const aSpd = AXES.a.stepsPerUnit;
        const aInv = AXES.a.invert ? -1 : 1;
        forEachFixture((name, curves) => {
            const p = planFor([curves], KNIFE);
            const netA = (prep([curves], KNIFE).reduce((a, s) => a + s.da * aInv, 0)) / aSpd;
            let turn = 0;
            for (const [lo, hi] of subpathRanges(p)) {
                let th = p[lo]!.theta;
                for (let i = lo; i < hi; i++) {
                    turn += angleDelta(th, p[i + 1]!.theta);
                    th = p[i + 1]!.theta;
                }
            }
            const want = p[0]!.theta + turn;
            // one A step of slack: da is rounded at every emit
            const slack = 2 / aSpd;
            return Math.abs(netA - want) > slack
                ? [`${name}: net A ${netA.toFixed(4)}deg, want ${want.toFixed(4)}deg`]
                : [];
        });
    });

    it("unwind keeps physical A bounded over repeated closed loops", () => {
        const circle = CASES.full_circle_r30!.curves;
        const aInv = AXES.a.invert ? -1 : 1;
        let phys = 0;
        let peak = 0;
        for (const s of prep([circle, circle, circle], KNIFE)) {
            phys += s.da * aInv;
            peak = Math.max(peak, Math.abs(phys));
        }
        expect(peak).toBeLessThan(540 * AXES.a.stepsPerUnit);
    });

    it("unwind stays correct when the winding came from corner pivots", () => {
        // The circles above wind A entirely through tracking. A square winds it
        // entirely through corner PIVOTS, which update aPhys on a separate code
        // path (discretize.ts:208). Repeat the square so a mis-tracked aPhys
        // compounds into the next subpath's pre-orientation instead of
        // cancelling within one.
        const square = [
            line({ x: 0, y: 0 }, { x: 20, y: 0 }),
            line({ x: 20, y: 0 }, { x: 20, y: 20 }),
            line({ x: 20, y: 20 }, { x: 0, y: 20 }),
            line({ x: 0, y: 20 }, { x: 0, y: 0 }),
        ];
        const aInv = AXES.a.invert ? -1 : 1;
        let phys = 0;
        let peak = 0;
        for (const s of prep([square, square, square], KNIFE)) {
            phys += s.da * aInv;
            peak = Math.max(peak, Math.abs(phys));
        }
        expect(peak).toBeLessThan(540 * AXES.a.stepsPerUnit);
    });
});

describe("stage 8 INVARIANT: every emitted segment is executable", () => {
    it("interval is an integer in [1, fCpu] on every segment", () => {
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const profile of [KNIFE, PEN]) {
                for (const [i, s] of prep([curves], profile).entries()) {
                    if (!Number.isInteger(s.interval) || s.interval < 1 || s.interval > MACH.fCpu) {
                        bad.push(`${name}/${profile.name}: seg ${i} interval ${s.interval}`);
                    }
                }
            }
            return bad.slice(0, 3);
        });
    });

    it("all step deltas are integers", () => {
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const [i, s] of prep([curves], KNIFE).entries()) {
                if (![s.dx, s.dy, s.dz, s.da].every(Number.isInteger)) {
                    bad.push(`${name}: seg ${i} non-integer delta`);
                }
            }
            return bad.slice(0, 3);
        });
    });
});

describe("stage 8 INVARIANT: PATH_END marks each subpath exactly once", () => {
    it("one MICRO_PATH_END per subpath, on a segment that moves", () => {
        const subpaths = [CASES.straight_line!.curves, CASES.quarter_circle_r5!.curves];
        const segs = prep(subpaths, KNIFE);
        const ends = segs.filter((s) => s.flags & MICRO_PATH_END);
        expect(ends.length).toBe(subpaths.length);
        for (const e of ends) expect(major(e)).toBeGreaterThan(0);
    });

    it("the final cutting segment of a single subpath carries it", () => {
        const segs = cutting(prep([CASES.straight_line!.curves], KNIFE));
        expect(segs[segs.length - 1]!.flags & MICRO_PATH_END).toBeTruthy();
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// CONTRACT PROPERTIES — tool behaviour
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 8: a non-tangential tool", () => {
    it("never rotates A and never lifts", () => {
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const s of prep([curves], PEN)) {
                if (s.da !== 0) bad.push(`${name}: da=${s.da}`);
                if (s.flags & MICRO_LIFT) bad.push(`${name}: MICRO_LIFT set`);
            }
            return bad.slice(0, 2);
        });
    });

    it("rounds a 90-degree corner at speed instead of stopping for it", () => {
        // The junction-deviation path: a pen has no blade to reorient, so a
        // sharp join is cornered, not lift-pivoted. Pins that PEN does NOT
        // inherit the knife's stop.
        const p = planFor([[line({ x: 0, y: 0 }, { x: 20, y: 0 }), line({ x: 20, y: 0 }, { x: 20, y: 20 })]], PEN);
        const bi = p.findIndex((s) => s.flags & 0x04);
        expect(bi).toBeGreaterThan(0);
        expect(p[bi]!.v).toBeGreaterThan(1.0);
        expect(prep([[line({ x: 0, y: 0 }, { x: 20, y: 0 }), line({ x: 20, y: 0 }, { x: 20, y: 20 })]], PEN)
            .some((s) => s.da !== 0)).toBe(false);
    });
});

describe("stage 8: a tangential tool at a corner", () => {
    it("emits a pure-A pivot at a 90-degree join", () => {
        const segs = prep([[line({ x: 0, y: 0 }, { x: 20, y: 0 }), line({ x: 20, y: 0 }, { x: 20, y: 20 })]], KNIFE);
        const pivots = segs.filter((s) => (s.flags & MICRO_JOG) && s.da !== 0 && s.dx === 0 && s.dy === 0);
        expect(pivots.length).toBeGreaterThan(0);
        // and the pivot turns through the full corner
        const turned = pivots.reduce((a, s) => a + s.da * (AXES.a.invert ? -1 : 1), 0) / AXES.a.stepsPerUnit;
        expect(Math.abs(turned)).toBeGreaterThan(80);
    });

    it("does not pivot where the tangent turns smoothly", () => {
        // A quarter circle turns 90 degrees in total but never more than
        // dthetaMax at once, so it must be tracked continuously, not pivoted.
        const p = planFor([CASES.quarter_circle_r50!.curves], KNIFE);
        expect(cornerIndices(p, KNIFE)).toEqual([]);
    });
});

describe("stage 8: per-axis invert is applied to every emitted delta", () => {
    // The default machine has x.invert = true but y.invert = false, so the
    // fixtures alone cannot tell "Y invert applied" from "Y invert ignored".
    // Flip each axis explicitly and require the emitted deltas to negate.
    function withInvert(axis: "x" | "y", invert: boolean) {
        return { ...MACH, [axis]: { ...MACH[axis], invert } };
    }

    it("flipping x.invert negates every dx and nothing else", () => {
        const p = planFor([CASES.s_curve!.curves], PEN);
        const a = discretize(p, withInvert("x", false), PEN, q);
        const b = discretize(p, withInvert("x", true), PEN, q);
        expect(a.length).toBe(b.length);
        for (let i = 0; i < a.length; i++) {
            expect(b[i]!.dx).toBe(-a[i]!.dx);
            expect(b[i]!.dy).toBe(a[i]!.dy);
        }
    });

    it("flipping y.invert negates every dy and nothing else", () => {
        const p = planFor([CASES.s_curve!.curves], PEN);
        const a = discretize(p, withInvert("y", false), PEN, q);
        const b = discretize(p, withInvert("y", true), PEN, q);
        expect(a.length).toBe(b.length);
        for (let i = 0; i < a.length; i++) {
            expect(b[i]!.dy).toBe(-a[i]!.dy);
            expect(b[i]!.dx).toBe(a[i]!.dx);
        }
    });
});

describe("stage 8: Z lift choreography", () => {
    // Every tool profile ships liftHeight = 0, so in the default config `lift`
    // is false and NOTHING in the Z path executes — no lower-to-cut, no
    // raise-at-end, no lift inside a corner pivot. The lift-pivot-lower that
    // the whole corner design rests on has never actually lifted under test.
    // These drive it through the documented liftHeight override.
    const LIFT = 2.0;
    const lifted = (subpaths: readonly (readonly CubicBezier[])[], profile: Profile) =>
        discretize(planFor(subpaths, profile), MACH, profile, q, { liftHeight: LIFT });

    it("lowers before the stroke and raises after it, by the same step count", () => {
        const segs = lifted([CASES.straight_line!.curves], KNIFE);
        const runs = zRuns(segs);
        expect(runs.length).toBe(2);
        expect(runs[0]!.dz + runs[1]!.dz).toBe(0); // returns to travel height
        expect(Math.abs(runs[0]!.dz)).toBe(Math.round(LIFT * AXES.z.stepsPerUnit));
        expect(runs[0]!.dz).toBe(-runs[1]!.dz); // down first, up last
    });

    it("net Z is zero over many subpaths — every lower is matched by a raise", () => {
        const segs = lifted(
            [CASES.straight_line!.curves, CASES.quarter_circle_r5!.curves, CUSP],
            KNIFE,
        );
        expect(segs.reduce((a, s) => a + s.dz, 0)).toBe(0);
    });

    it("a corner pivot lifts, turns, and lowers again", () => {
        const corner = [line({ x: 0, y: 0 }, { x: 20, y: 0 }), line({ x: 20, y: 0 }, { x: 20, y: 20 })];
        const segs = lifted([corner], KNIFE);
        // The pivot's Z pair is interior: strip the leading lower and trailing raise.
        const runs = zRuns(segs);
        // four runs: lower to cut, the pivot's lift and lower, raise after
        expect(runs.length).toBe(4);
        const [lift, lower] = [runs[1]!, runs[2]!];
        expect(lift.dz).toBe(-lower.dz); // the pivot's pair cancels
        // and a pure-A rotation happens between the lift and the lower
        const between = segs.slice(lift.to + 1, lower.from);
        expect(between.length).toBeGreaterThan(0);
        expect(between.every((s) => s.dx === 0 && s.dy === 0 && s.dz === 0)).toBe(true);
        expect(between.some((s) => s.da !== 0)).toBe(true);
    });

    it("XY conservation is unaffected by lifting", () => {
        const [nx, ny] = net(lifted([CASES.s_curve!.curves], KNIFE));
        expect([nx, ny]).toEqual(expectedXY([CASES.s_curve!.curves]));
    });
});

/**
 * Maximal runs of consecutive Z-moving segments, with each run's signed total.
 *
 * A lift is a RAMP now (H3), not one segment, so "the lower before the stroke"
 * is a contiguous group rather than a single index. Grouping is what keeps
 * these tests stating the property — down, then up, matched — instead of
 * counting emitter internals that the ramp granularity is free to change.
 */
function zRuns(segs: readonly MicroSegment[]): { from: number; to: number; dz: number }[] {
    const runs: { from: number; to: number; dz: number }[] = [];
    for (let i = 0; i < segs.length; i++) {
        if (segs[i]!.dz === 0) continue;
        const from = i;
        let dz = 0;
        while (i < segs.length && segs[i]!.dz !== 0) dz += segs[i++]!.dz;
        runs.push({ from, to: i - 1, dz });
    }
    return runs;
}

describe("stage 8: velocity-aware subdivision", () => {
    it("skips interior sub-steps that move nothing", () => {
        // The interior-skip guard (discretize.ts:177) working as intended.
        // Interior only — the guard's two exemptions are D1's subject.
        const p = planFor([CASES.long_gentle_arc!.curves], PEN);
        const dense = discretize(p, MACH, PEN, { ...q, dvMax: 0.05 });
        const zeros = dense.map((s, i) => [s, i] as const).filter(([s]) => major(s) === 0);
        for (const [s] of zeros) {
            expect(s.flags & MICRO_PATH_END).toBeTruthy(); // only the exempted final one
        }
    });

    it("no cutting segment spans a speed change greater than dvMax", () => {
        forEachFixture((name, curves) => {
            const p = planFor([curves], KNIFE);
            const bad: string[] = [];
            for (const [lo, hi] of subpathRanges(p)) {
                for (let i = lo; i < hi; i++) {
                    const dv = Math.abs(p[i + 1]!.v - p[i]!.v);
                    const k = Math.min(256, Math.max(1, Math.ceil(dv / q.dvMax)));
                    if (dv / k > q.dvMax * 1.001) {
                        bad.push(`${name}: pair ${i} realised dv ${(dv / k).toFixed(3)} > ${q.dvMax}`);
                    }
                }
            }
            return bad.slice(0, 3);
        });
    });

    it("a cruise at constant speed is not subdivided", () => {
        // k=1 on cruise is what keeps segment counts sane; if this regresses the
        // wire volume explodes without improving anything.
        const mid = cutting(prep([CASES.long_gentle_arc!.curves], PEN));
        const p = planFor([CASES.long_gentle_arc!.curves], PEN);
        const cruisePairs = [...subpathRanges(p)].flatMap(([lo, hi]) => {
            const n: number[] = [];
            for (let i = lo; i < hi; i++) if (Math.abs(p[i + 1]!.v - p[i]!.v) < q.dvMax) n.push(i);
            return n;
        });
        expect(cruisePairs.length).toBeGreaterThan(100);
        expect(mid.length).toBeLessThan(p.length * 1.5);
    });
});

// ══════════════════════════════════════════════════════════════════════════════
// FINDINGS — these fail. Each pins a defect recorded in docs/planner_audit.md.
// ══════════════════════════════════════════════════════════════════════════════

describe("stage 8 D1 (FIXED): no empty segment carrying a one-second interval", () => {
    // The interior-skip guard used to exempt two cases from being
    // skipped: the subpath's final sub-step, and a corner's last sub-step. Both
    // can have every delta zero — and interval()'s `if (major === 0) return
    // fCpu` then hands the empty segment the largest interval representable:
    //
    //     dx=dy=dz=da=0, interval = 150,000,000 = a full second at fCpu.
    //
    // Both exemptions are reachable, and neither needs strange geometry:
    //
    //   corner  — `cusp` fixture with a KNIFE. dx/dy are zero because the two
    //             samples are coincident; da is zero because the tracking
    //             branch is gated on `!isCorner`.
    //   final   — `long_gentle_arc` with a PEN at dvMax = 0.05. The last
    //             sub-step rounds to no motion, and the segment is emitted
    //             anyway because it carries MICRO_PATH_END. So the marker that
    //             ENDS every path is itself the empty one.
    //
    // Whether the firmware stalls a second on a zero-step segment or discards
    // it is a wire-contract question this stage should not be leaving open, and
    // the PATH_END case makes it reachable on ordinary work.
    //
    // FIXED (batch B): every zero-motion sub-step is skipped, including those
    // two. PATH_END is not dropped with them — it is re-homed onto the last
    // segment the subpath actually emitted, which is the same position in the
    // stream minus the empty second. The one-PATH_END-per-subpath invariant
    // (asserted in the INVARIANTS block) is what pins that it survives; the
    // tests here pin WHERE it lands.
    it("emits no segment with zero motion on any axis", () => {
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const profile of [KNIFE, PEN]) {
                for (const [i, s] of prep([curves], profile).entries()) {
                    if (major(s) === 0) {
                        bad.push(`${name}/${profile.name}: seg ${i} all-zero, interval ${s.interval} (${(s.interval / MACH.fCpu).toFixed(3)}s)`);
                    }
                }
            }
            return bad;
        });
    });

    it("reaches the PATH_END marker on ordinary geometry, not just a cusp", () => {
        // The exemption that matters most: this is a pen on a gentle arc.
        const p = planFor([CASES.long_gentle_arc!.curves], PEN);
        const dense = discretize(p, MACH, PEN, { ...q, dvMax: 0.05 });
        const empty = dense.filter((s) => major(s) === 0);
        expect(empty.length).toBe(0);
    });

    it("PATH_END lands on a segment that moves, on every fixture and tool", () => {
        // The relocation's actual contract. Without this, skipping the final
        // sub-step could be 'fixed' by dropping the marker onto anything.
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const profile of [KNIFE, PEN]) {
                for (const [i, s] of prep([curves], profile).entries()) {
                    if ((s.flags & MICRO_PATH_END) !== 0 && major(s) === 0) {
                        bad.push(`${name}/${profile.name}: PATH_END on empty seg ${i}`);
                    }
                }
            }
            return bad;
        });
    });

    it("keeps PATH_END on the last cutting segment when the final sub-step moves", () => {
        // The common case must be untouched by the relocation: when the final
        // sub-step does move, the marker rides it, exactly as before.
        const segs = prep([[line({ x: 0, y: 0 }, { x: 40, y: 0 })]], PEN);
        const cut = cutting(segs);
        expect(cut.length).toBeGreaterThan(1);
        expect(cut[cut.length - 1]!.flags & MICRO_PATH_END).toBe(MICRO_PATH_END);
        expect(cut.slice(0, -1).some((s) => (s.flags & MICRO_PATH_END) !== 0)).toBe(false);
    });

    it("marks the last CUTTING segment, not the last segment, when a subpath ends on a corner", () => {
        // A subpath whose final pair is a corner emits pivot (and Z-raise)
        // segments AFTER the cut ends: measured 52 segments after the marker.
        // So "last segment emitted" and "last cutting segment" genuinely differ
        // here, and PATH_END belongs to the cut. This is what stops the D1
        // relocation from drifting onto the choreography that follows.
        const tinyTail = [
            line({ x: 0, y: 0 }, { x: 10, y: 0 }),
            line({ x: 10, y: 0 }, { x: 10.001, y: 0.001 }),
        ];
        const segs = prep([tinyTail], KNIFE);
        const at = segs.findIndex((s) => (s.flags & MICRO_PATH_END) !== 0);
        expect(at).toBeGreaterThanOrEqual(0);
        expect(segs.length - 1 - at).toBeGreaterThan(0); // choreography follows it
        expect(major(segs[at]!)).toBeGreaterThan(0);
        // and everything after it is non-cutting (lift / pivot / lower / raise)
        expect(cutting(segs).indexOf(segs[at]!)).toBe(cutting(segs).length - 1);
    });

    it("moves PATH_END back one segment when the final sub-step is empty", () => {
        // The relocation firing, isolated: the dense PEN arc above is the case
        // where the last sub-step rounds to no motion. The marker must be on the
        // segment before it, and that segment must be a real move.
        const p = planFor([CASES.long_gentle_arc!.curves], PEN);
        const dense = cutting(discretize(p, MACH, PEN, { ...q, dvMax: 0.05 }));
        const last = dense[dense.length - 1]!;
        expect(last.flags & MICRO_PATH_END).toBe(MICRO_PATH_END);
        expect(major(last)).toBeGreaterThan(0);
    });
});

describe("stage 8 D2 (FIXED): sub-segment speed follows constant acceleration", () => {
    // discretize.ts:183-185 interpolates the sub-segment speed linearly in
    // ARC LENGTH:  v(f) = v0 + (v1 - v0) * f.
    //
    // Under constant acceleration — which is exactly what plan's sweeps
    // produce — speed is not linear in distance. It is
    //     v(s) = sqrt(v0^2 + 2*a*s),   i.e.  v(f) = sqrt(v0^2 + f*(v1^2 - v0^2)).
    //
    // Consequences, measured:
    //   - At k=1 the pair-level mean (v0+v1)/2 is EXACTLY right for constant
    //     accel, so the emitted time is exact: ratio 1.0000.
    //   - Every subdivision replaces that one exact estimate with k wrong ones,
    //     and the error grows monotonically the harder it subdivides:
    //         10mm line, dvMax = inf / 24 / 6 / 3 / 0.75
    //                    1.000 / 1.103 / 1.268 / 1.361 / 1.510
    //     Subdivision exists to improve fidelity (premortem P3). For timing it
    //     does the opposite, and the knob that is supposed to buy accuracy is
    //     the knob that costs it.
    //   - Over a pair leaving rest the linear model's time integral diverges
    //     logarithmically (v0 = 0 -> infinite). quality.vMin is the only reason
    //     the number is finite; that clamp is load-bearing by accident.
    //
    // FIXED (batch B): v is interpolated as sqrt(v0^2 + f*(v1^2 - v0^2)), so
    // each sub-segment's own mean is exact and the sub-times sum back to the
    // undivided pair time. The tests below are now the CONTRACT: subdivision
    // must be timing-neutral. They were written as red finding tests against
    // the linear model, and inverting them is the whole record of the fix.
    //
    // Measured on the fixture set, emitted / exact cut time:
    //     short_curve  1.215 -> 1.000
    //     cusp         1.132 -> 1.088   (residual is D3, not D2)
    //     near_cusp    1.909 -> 1.861   (residual is D3, not D2)
    //
    // The exempt set is not a judgement call: cusp and near_cusp are EXACTLY
    // the two fixtures on which the plan asks the A axis for more than its rate
    // ceiling (16.82x and 1.02x), which is the precondition for interval()'s
    // floor to stretch a segment. Every fixture where D3 cannot fire is exact.
    const D3_STRETCHED = new Set(["cusp", "near_cusp"]);

    it("emitted cut time matches the exact constant-accel time", () => {
        forEachFixture((name, curves) => {
            if (D3_STRETCHED.has(name)) return []; // asserted red under D3
            const p = planFor([curves], KNIFE);
            const ratio = emittedSeconds(cutting(prep([curves], KNIFE))) / plannedSeconds(p);
            return ratio > 1.02
                ? [`${name}: emitted ${ratio.toFixed(3)}x the exact cut time`]
                : [];
        });
    });

    it("subdivision is timing-neutral: dvMax buys fidelity without costing time", () => {
        // The isolation that identified the cause, now inverted. Runs a PEN, so
        // no A axis: any error would be in the sub-segment model alone, not in
        // interval(), not in step rounding.
        const p = planFor([[line({ x: 0, y: 0 }, { x: 10, y: 0 })]], PEN);
        const exact = plannedSeconds(p);
        const ratioAt = (dvMax: number) =>
            emittedSeconds(discretize(p, MACH, PEN, { ...q, dvMax })) / exact;

        expect(ratioAt(1e9)).toBeCloseTo(1.0, 3); // k=1 everywhere
        expect(ratioAt(6)).toBeCloseTo(1.0, 2);   // was 1.268
        expect(ratioAt(0.75)).toBeCloseTo(1.0, 2); // was 1.510 — the harder it
        // subdivides the worse it used to get; monotonic degradation is gone.
    });

    it("times a ramp-dominated path as accurately as a cruising one", () => {
        // Long paths cruise, so the old ramp error was diluted; short ones are
        // all ramp. This asymmetry is why the golden fixtures (long SVG paths)
        // never showed D2 and a 10mm line did. It must no longer exist.
        const ratioFor = (L: number) => {
            const p = planFor([[line({ x: 0, y: 0 }, { x: L, y: 0 })]], PEN);
            return emittedSeconds(discretize(p, MACH, PEN, q)) / plannedSeconds(p);
        };
        expect(ratioFor(10)).toBeCloseTo(1.0, 2);  // was 1.361
        expect(ratioFor(500)).toBeCloseTo(1.0, 2);
    });

    it("leaving rest is finite without leaning on vMin", () => {
        // The linear model's time integral over a pair is ds*ln(v1/v0)/(v1-v0),
        // which diverges as v0 -> 0: quality.vMin was the only reason a ramp
        // off a standstill produced a finite number, and that made an accuracy
        // clamp load-bearing for termination. Under sqrt interpolation the mean
        // is (v0+v1)/2 with v0 = 0 handled exactly, so slashing vMin by 1000x
        // must barely move the emitted time.
        const p = planFor([[line({ x: 0, y: 0 }, { x: 10, y: 0 })]], PEN);
        const at = (vMin: number) =>
            emittedSeconds(discretize(p, MACH, PEN, { ...q, vMin }));
        expect(at(q.vMin / 1000) / at(q.vMin)).toBeCloseTo(1.0, 2);
    });
});

describe("stage 8 D3 (RESOLVED, doc): the plan is a velocity schedule, not a clock", () => {
    // interval() floors each segment's duration so no axis exceeds
    // maxFeed * stepsPerUnit. The floor is correct and necessary, and it is
    // applied AFTER planning, so the executed timeline is slower than the
    // planned one wherever it binds. That divergence is REAL and is not going
    // to be fixed: see docs/planner_audit.md D3. It is accepted rather than
    // repaired because nothing consumes plan time. Stage 9's duty windows were
    // the claimed stake and they do not — dutyBreaks.ts:49 measures EMITTED
    // segments. The plan is consumed as a velocity schedule; only the emitted
    // stream is consumed as a clock.
    //
    // Four candidate root causes were proposed and eliminated (audit doc D3):
    // constrain's A-slew cap (inert when implemented), flatten's refTheta (turn
    // is conserved exactly), |da| quantisation, and trunc bias in interval
    // (0.006% at worst — the interval LSB is 6.7ns against intervals of
    // thousands of cycles). Three mechanisms DO account for it, quantified as
    // D3a/D3b/D3c in the doc. None is a defect in isolation.
    //
    // So the tests below pin the divergence instead of demanding it vanish:
    // T2 bounds it per fixture, T3 asserts the invariant interval() actually
    // owes us. Both were mutation-tested (6 mutants, all killed) before being
    // trusted; the exercise found two defects in the tests themselves.
    //
    // Do NOT relax a T2 bound to make a change pass. A bound that moves is a
    // behaviour change and belongs in the audit doc with a measurement.

    // The plan's own A rate. This passes today — which is itself the retraction
    // of the original D3 root-cause chain, which claimed the plan overdrives
    // the A ceiling by 16.8x. It does not; the A-rate story was a red herring
    // (audit doc, superseded chain #1). Kept as a forward invariant.
    it("the plan never asks the A axis for more than its rate ceiling", () => {
        forEachFixture((name, curves) => {
            const p = planFor([curves], KNIFE);
            let worst = 0;
            for (const [lo, hi] of subpathRanges(p)) {
                for (let i = lo; i < hi; i++) {
                    const a = p[i]!;
                    const b = p[i + 1]!;
                    if (a.ds < 1e-9 || a.v < 1e-9) continue;
                    const dt = a.ds / (0.5 * (a.v + b.v));
                    worst = Math.max(worst, Math.abs(angleDelta(a.theta, b.theta)) / dt);
                }
            }
            return worst > AXES.a.maxFeed * 1.01
                ? [`${name}: plan asks A for ${worst.toFixed(0)} deg/s (${(worst / AXES.a.maxFeed).toFixed(2)}x the ${AXES.a.maxFeed} ceiling)`]
                : [];
        });
    });

    // T2 — bound the divergence per fixture. Every fixture where none of
    // D3a/D3b/D3c can fire executes its plan's timeline to within 0.71%; the
    // two where they do are exempted at their MEASURED value plus headroom.
    // Those two numbers are the acknowledgement: this is how far apart the
    // schedule and the clock are, and we know why for each.
    const T2_BOUND: Record<string, number> = {
        cusp: 1.12,      // measured 1.088 — D3c (XY step quantisation) dominant
        near_cusp: 1.90, // measured 1.861 — D3a (zero-length rotation) is 89% of it
    };

    it("plan-vs-emitted time divergence stays inside its measured bound", () => {
        forEachFixture((name, curves) => {
            const p = planFor([curves], KNIFE);
            const ratio = emittedSeconds(cutting(prep([curves], KNIFE))) / plannedSeconds(p);
            const bound = T2_BOUND[name] ?? 1.02;
            return ratio > bound
                ? [`${name}: emitted ${ratio.toFixed(3)}x planned, bound ${bound}`]
                : [];
        });
    });

    // T3 — the invariant interval() exists to guarantee. Computed from emitted
    // integers only: a BOUND on a ratio, never a difference, so Q4/Q5
    // quantisation cannot amplify into it (audit doc, "where to measure").
    it("no axis is clocked faster than maxFeed * stepsPerUnit", () => {
        forEachFixture((name, curves) => {
            const bad: string[] = [];
            for (const s of cutting(prep([curves], KNIFE))) {
                const m = major(s);
                if (m === 0) continue;
                const dt = (s.interval * m) / MACH.fCpu;
                // interval() truncs, so the emitted duration may fall short of
                // the exact requirement by up to one clock tick per major step.
                const slack = 1 + 1 / s.interval;
                for (const [d, ax, nm] of [
                    [s.dx, AXES.x, "x"], [s.dy, AXES.y, "y"],
                    [s.dz, AXES.z, "z"], [s.da, AXES.a, "a"],
                ] as const) {
                    const ceiling = ax.maxFeed * ax.stepsPerUnit;
                    if (ceiling <= 0 || d === 0) continue;
                    const rate = Math.abs(d) / dt;
                    if (rate > ceiling * slack) {
                        bad.push(`${name} ${nm}: ${rate.toFixed(0)} > ${ceiling.toFixed(0)} st/s`);
                    }
                }
            }
            return bad.slice(0, 3);
        });
    });

    it("T3 is not vacuous: the ceiling is actually approached", () => {
        // T3 is one-sided (the C3/H6 shape): a change that makes everything
        // slower passes it comfortably. This asserts the bound has something to
        // bound.
        //
        // Feed-governed segments ONLY. interval()'s pure-rotation branch returns
        // tRate exactly, so those sit on the ceiling BY CONSTRUCTION and would
        // satisfy this guard no matter how badly the feed path degrades. That is
        // not hypothetical: with every feed segment mutated 2x slow this read
        // 1.000 including them and 0.500 excluding them. Excluding them is what
        // makes this test do its job.
        let closest = 0;
        for (const curves of Object.values(GEOMETRY_CASES)) {
            for (const s of cutting(prep([curves], KNIFE))) {
                const m = major(s);
                if (m === 0 || (s.dx === 0 && s.dy === 0)) continue;
                const dt = (s.interval * m) / MACH.fCpu;
                for (const [d, ax] of [
                    [s.dx, AXES.x], [s.dy, AXES.y], [s.da, AXES.a],
                ] as const) {
                    const ceiling = ax.maxFeed * ax.stepsPerUnit;
                    if (ceiling <= 0 || d === 0) continue;
                    closest = Math.max(closest, Math.abs(d) / dt / ceiling);
                }
            }
        }
        expect(closest).toBeGreaterThan(0.95);
    });

    it("documents the root cause: actual turn exceeds what kappa predicts", () => {
        // Passes today. constrain's A-slew cap is rad(aRate)/kappa, which is
        // only sound if the sample-to-sample turn equals kappa*ds. It does not.
        // Delete this only together with F7.
        const p = planFor([CUSP], KNIFE);
        let worst = 0;
        for (const [lo, hi] of subpathRanges(p)) {
            for (let i = lo; i < hi; i++) {
                const a = p[i]!;
                const predicted = (a.kappa * a.ds * 180) / Math.PI;
                if (predicted > 1e-9) {
                    worst = Math.max(worst, Math.abs(angleDelta(a.theta, p[i + 1]!.theta)) / predicted);
                }
            }
        }
        expect(worst).toBeGreaterThan(4);
    });
});

describe("stage 8 FINDING D4: the corner rule is ungated, unlike constrain's", () => {
    // constrain gates its corner-stop on (flags & CURVE_BOUNDARY); discretize
    // gates on nothing (discretize.ts:137-138). So discretize will lift-pivot at
    // an INTRA-curve tangent jump that constrain never stopped for, contradicting
    // this stage's own header ("velocity planning already brought the tool to
    // v=0 at every corner").
    //
    // Measured, the gap is currently narrow: the only geometry that reaches it
    // is a cusp, where the curvature caps happen to have crawled v down anyway
    // (4.8e-3 mm/s on `cusp`, 4.8e-2 on a longer-armed variant). So today the
    // precondition holds BY ACCIDENT, not by construction — and interval()
    // floors that crawl up to vMin = 0.5 mm/s regardless, so the pivot does
    // execute while moving.
    //
    // It is filed rather than dismissed because the accident is F1's doing: a
    // cusp is exactly where flatten's tangent cap is skipped. Fix F1 so the
    // marcher resolves cusps properly and this stops being a cusp-only case.
    it("every corner it pivots at was stopped for by constrain", () => {
        forEachFixture((name, curves) => {
            const p = planFor([curves], KNIFE);
            const bad: string[] = [];
            for (const i of cornerIndices(p, KNIFE)) {
                if (p[i]!.vCeiling !== 0) {
                    bad.push(`${name}: corner at sample ${i} has vCeiling ${p[i]!.vCeiling.toExponential(2)}, not 0 (CURVE_BOUNDARY=${!!(p[i + 1]!.flags & 0x04)})`);
                }
            }
            return bad.slice(0, 3);
        });
    });
});
