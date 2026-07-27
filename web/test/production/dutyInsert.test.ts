/**
 * Step 1 — the SPEC for inserted lifts (docs/tool_duty_limits.md §5 tier 2).
 *
 * Today a stretch of toolpath with no lift inside the band throws, because
 * breaking it means inserting a stop where the planner did not plan one. This
 * file pins down what "implemented" will mean, and guards the current
 * behaviour in the meantime so the throw cannot be lost by accident.
 *
 * The positive specs are `.skip`ped, not deleted: they are the acceptance
 * criteria for tier 2, and unskipping them is the definition of done. A red
 * suite on main would be worse than a documented gap.
 */

import { describe, it, expect } from "vitest";
import { scheduleDutyBreaks } from "../../src/production/dutyBreaks.js";
import {
    microSegment,
    MICRO_LIFT,
    MICRO_PAUSE,
    MICRO_DUTY_RELEASE,
    MICRO_DUTY_ASSERT,
    type MicroSegment,
} from "../../src/wire/format/microsegment.js";
import { resolvedAxes } from "../../src/config/config.js";
import { uniformMachine } from "../../src/config/fixtures.js";
import type { DutyLimits } from "../../src/config/schema.js";

const F_CPU = 1e6;
const axes = resolvedAxes(uniformMachine(160, 45.46, { fCpu: F_CPU }));
const duty: DutyLimits = { maxOnS: 30, minOnS: 20, dwellS: 2, settleS: 0 };

/** One unbroken 60 s cut: no lift anywhere in the band. */
const liftFree = (): MicroSegment[] => [
    microSegment(60_000, 0, 0, 0, 1000, 0),
    microSegment(0, 0, 100, 0, 100, MICRO_LIFT),
];

describe("inserted lifts — current behaviour", () => {
    it("refuses the job rather than overrunning the duty budget", () => {
        expect(() => scheduleDutyBreaks(liftFree(), duty, axes, F_CPU, "knife")).toThrow(
            /no lift between/,
        );
    });
});

describe.skip("inserted lifts — tier 2 acceptance criteria", () => {
    // Each of these will need the post-pass to become a pipeline loop, so
    // scheduleDutyBreaks' signature will change (it must be able to re-run
    // constrain→plan→discretize). Written against behaviour, not signature,
    // so they survive that.

    it("inserts a lift when the band holds none", () => {
        const r = scheduleDutyBreaks(liftFree(), duty, axes, F_CPU, "knife");
        expect(r.breaksAtS.length).toBeGreaterThanOrEqual(1);
    });

    it("every scheduled break still lands on a raise carrying PAUSE", () => {
        const r = scheduleDutyBreaks(liftFree(), duty, axes, F_CPU, "knife");
        const marked = r.segments.filter((s) => s.flags & MICRO_DUTY_RELEASE);
        expect(marked.length).toBe(r.breaksAtS.length);
        for (const s of marked) {
            expect(s.flags & MICRO_LIFT).toBeTruthy();
            expect(s.flags & MICRO_PAUSE).toBeTruthy();
            expect(s.flags & MICRO_DUTY_ASSERT).toBeTruthy();
        }
    });

    it("respects the band — no two resets closer than minOnS, none past maxOnS", () => {
        const r = scheduleDutyBreaks(liftFree(), duty, axes, F_CPU, "knife");
        const stamps = [0, ...r.breaksAtS];
        for (let i = 1; i < stamps.length; i++) {
            const span = stamps[i]! - stamps[i - 1]!;
            expect(span).toBeGreaterThan(duty.minOnS);
            expect(span).toBeLessThanOrEqual(duty.maxOnS);
        }
    });

    it("an inserted stop costs a decel and an accel, so the job gets LONGER", () => {
        // The one thing an inserted lift can never be is free. Guarding the
        // direction catches a stop that was marked but not actually planned
        // into the motion — the failure mode that would crash a real knife.
        const before = liftFree();
        const r = scheduleDutyBreaks(before, duty, axes, F_CPU, "knife");
        const secs = (ss: readonly MicroSegment[]) =>
            ss.reduce(
                (a, s) =>
                    a +
                    (Math.max(Math.abs(s.dx), Math.abs(s.dy), Math.abs(s.dz), Math.abs(s.da)) *
                        s.interval) /
                        F_CPU,
                0,
            );
        expect(secs(r.segments)).toBeGreaterThan(secs(before));
    });
});
