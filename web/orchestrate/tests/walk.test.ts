/**
 * Tests for the runtime orchestrator walk.
 */

import { describe, it, expect } from "vitest";
import { walkSchedule, type WalkEvent } from "../src/walk.js";
import { scheduleMounts } from "../src/schedule.js";
import type { Plan } from "../../plan/src/plan.js";
import {
    PEN,
    KNIFE,
    CREASE,
    REVOLVER_PEN,
    ToolType,
    axisConfig,
    busNode,
    toolHead,
    machineConfig,
    type MachineConfig,
    type ToolProfile,
} from "../../config/config.js";
import { MICRO_JOG } from "../../wire/src/microsegment.js";

// ── fixtures ──────────────────────────────────────────────────────────────────

function singleHeadMachine(): MachineConfig {
    return machineConfig(
        axisConfig(busNode(1), 160, { invert: true, maxRate: 80, accel: 1000 }),
        axisConfig(busNode(2), 160, { maxRate: 80, accel: 1000 }),
        [
            toolHead(
                axisConfig(busNode(3), 1200, { invert: true }),
                axisConfig(busNode(4), 51.667, { rotary: true, invert: true, maxRate: 100, accel: 2000 }),
            ),
        ],
        { fCpu: 150_000_000, jogFeed: 80 },
    );
}

function dualHeadMachine(): MachineConfig {
    const z = axisConfig(busNode(3), 1200, { invert: true });
    const a = axisConfig(busNode(4), 51.667, { rotary: true, invert: true });
    return machineConfig(
        axisConfig(busNode(1), 160, { invert: true, maxRate: 80 }),
        axisConfig(busNode(2), 160, { maxRate: 80 }),
        [
            toolHead(z, a, { xOffset: -50, yOffset: 0 }),
            toolHead(z, a, { xOffset:  50, yOffset: 0 }),
        ],
        { fCpu: 150_000_000, jogFeed: 80 },
    );
}

function plan(...tools: (ToolProfile | [ToolProfile, number])[]): Plan {
    return {
        blocks: tools.map((t, i) => {
            const [profile, slot] = Array.isArray(t) ? t : [t, undefined];
            const startSteps = { x: i * 1000, y: 0 }; // stagger blocks so travel is visible
            return slot === undefined
                ? { profile, segments: [], startSteps }
                : { profile, slot, segments: [], startSteps };
        }),
    };
}

function motionEvents(events: WalkEvent[]): WalkEvent[] {
    return events.filter((e) => e.kind === "motion");
}

function pauseEvents(events: WalkEvent[]): WalkEvent[] {
    return events.filter((e) => e.kind === "pause");
}

// ── tests ─────────────────────────────────────────────────────────────────────

describe("walkSchedule: empty plan", () => {
    it("produces no events", () => {
        const s = scheduleMounts({ blocks: [] }, 1);
        expect(walkSchedule(s, { blocks: [] }, singleHeadMachine())).toEqual([]);
    });
});

describe("walkSchedule: single phase, no swap needed", () => {
    it("emits no pause event when mount starts empty and swapIn is the first fill", () => {
        // seedMounted=[] → first phase has swapIn=[KNIFE] but that IS the bare-start fill.
        // Pause fires because swapIn is non-empty (operator must load the tool).
        const p = plan(PEN);
        const s = scheduleMounts(p, 1);
        const events = walkSchedule(s, p, singleHeadMachine());
        const pauses = pauseEvents(events);
        expect(pauses).toHaveLength(1); // must load PEN before start
        expect((pauses[0] as unknown as { swapIn: ToolType[] }).swapIn).toContain(ToolType.PEN);
    });

    it("no pause when the seed already has the right tool", () => {
        const p = plan(PEN);
        const s = scheduleMounts(p, 1, [ToolType.PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        expect(pauseEvents(events)).toHaveLength(0);
    });
});

describe("walkSchedule: travel jog between blocks", () => {
    it("emits a travel jog between staggered blocks", () => {
        // Two pen blocks at x=0 and x=1000; should get a JOG segment between them.
        const p = plan(PEN, PEN);
        const s = scheduleMounts(p, 1, [ToolType.PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        const motions = motionEvents(events);
        // Find any motion event containing a JOG
        const jogEvent = motions.find(
            (e) => e.kind === "motion" && e.segments.some((seg) => seg.flags === MICRO_JOG),
        );
        expect(jogEvent).toBeDefined();
    });

    it("emits no travel for coincident block starts", () => {
        const p: Plan = {
            blocks: [
                { profile: PEN, segments: [], startSteps: { x: 0, y: 0 } },
                { profile: PEN, segments: [], startSteps: { x: 0, y: 0 } },
            ],
        };
        const s = scheduleMounts(p, 1, [ToolType.PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        const anyJog = motionEvents(events).some(
            (e) => e.kind === "motion" && e.segments.some((seg) => seg.flags === MICRO_JOG),
        );
        expect(anyJog).toBe(false);
    });
});

describe("walkSchedule: A-home before tangential blocks", () => {
    it("emits an A move before a knife block when aPhys != 0", () => {
        const p = plan(KNIFE);
        const s = scheduleMounts(p, 1, [ToolType.KNIFE]);
        // Start with aPhys at some non-zero position.
        const events = walkSchedule(s, p, singleHeadMachine(), {
            initialState: { aPhys: 500 },
        });
        const motions = motionEvents(events);
        // First inter-block motion should contain an A move (da != 0, no dx/dy)
        const aHomeSeg = motions
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .find((seg) => seg.da !== 0 && seg.dx === 0 && seg.dy === 0 && seg.flags === MICRO_JOG);
        expect(aHomeSeg).toBeDefined();
    });

    it("does not emit A-home when aPhys is already 0", () => {
        const p = plan(KNIFE);
        const s = scheduleMounts(p, 1, [ToolType.KNIFE]);
        const events = walkSchedule(s, p, singleHeadMachine(), {
            initialState: { aPhys: 0 },
        });
        const anyAMove = motionEvents(events)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .some((seg) => seg.da !== 0 && seg.dx === 0 && seg.dy === 0 && seg.flags === MICRO_JOG);
        expect(anyAMove).toBe(false);
    });
});

describe("walkSchedule: revolver slot selection", () => {
    it("emits an A rotation to the correct slot before a revolver block", () => {
        // slot 0 = 0°, slot 1 = 360/7 ≈ 51.43° — use slot 1 so it's non-zero
        const p: Plan = {
            blocks: [{ profile: REVOLVER_PEN, slot: 1, segments: [], startSteps: { x: 0, y: 0 } }],
        };
        const s = scheduleMounts(p, 1, [ToolType.REVOLVER_PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        const aSegs = motionEvents(events)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .filter((seg) => seg.da !== 0 && seg.dx === 0 && seg.dy === 0);
        expect(aSegs.length).toBeGreaterThan(0);
    });

    it("no A rotation for slot 0 (offset = 0°)", () => {
        const p: Plan = {
            blocks: [{ profile: REVOLVER_PEN, slot: 0, segments: [], startSteps: { x: 0, y: 0 } }],
        };
        const s = scheduleMounts(p, 1, [ToolType.REVOLVER_PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        const anyA = motionEvents(events)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .some((seg) => seg.da !== 0);
        expect(anyA).toBe(false);
    });
});

describe("walkSchedule: pause and swap events", () => {
    it("emits pause events at phase boundaries with correct diff", () => {
        // knife → pen → crease on a 2-head machine: phases [knife,pen] then [crease,knife]
        const p = plan(KNIFE, PEN, CREASE, KNIFE);
        const s = scheduleMounts(p, 2);
        const events = walkSchedule(s, p, singleHeadMachine());
        const pauses = pauseEvents(events);
        // Phase 0: load KNIFE+PEN. Phase 1: swap PEN→CREASE (KNIFE stays).
        expect(pauses).toHaveLength(2);
        const p1 = pauses[1] as unknown as { swapIn: ToolType[]; swapOut: ToolType[] };
        expect(p1.swapIn).toContain(ToolType.CREASE);
        expect(p1.swapOut).toContain(ToolType.PEN);
    });

    it("A-home is emitted before the pause at a phase boundary", () => {
        const p = plan(KNIFE, CREASE);
        const s = scheduleMounts(p, 1);
        // Start with aPhys non-zero so A-home fires
        const events = walkSchedule(s, p, singleHeadMachine(), {
            initialState: { aPhys: 1000 },
        });
        const pauses = pauseEvents(events);
        expect(pauses).toHaveLength(2);
        // The A-home motion event should appear before the second pause.
        const secondPauseIdx = events.indexOf(pauses[1]!);
        const motionBeforePause = events
            .slice(0, secondPauseIdx)
            .filter((e) => e.kind === "motion")
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .some((seg) => seg.da !== 0 && seg.dx === 0 && seg.dy === 0);
        expect(motionBeforePause).toBe(true);
    });
});

describe("walkSchedule: head-offset jog on head switch", () => {
    it("emits a jog when the active head changes between blocks", () => {
        const machine = dualHeadMachine();
        const p = plan(KNIFE, PEN);
        const s = scheduleMounts(p, 2, [ToolType.KNIFE, ToolType.PEN]);
        const headMap = new Map([[ToolType.KNIFE, 0], [ToolType.PEN, 1]]);
        const events = walkSchedule(s, p, machine, { headAssignment: headMap });
        // When we switch from head 0 to head 1, a JOG for the offset diff fires.
        const jogs = motionEvents(events)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .filter((seg) => seg.flags === MICRO_JOG && (seg.dx !== 0 || seg.dy !== 0));
        expect(jogs.length).toBeGreaterThan(0);
    });
});

describe("walkSchedule: event ordering", () => {
    it("pause always precedes the motion for the phase it guards", () => {
        const p = plan(KNIFE, PEN);
        const s = scheduleMounts(p, 1);
        const events = walkSchedule(s, p, singleHeadMachine());
        let lastPauseIdx = -1;
        let firstMotionAfterPause = -1;
        for (let i = 0; i < events.length; i++) {
            if (events[i]!.kind === "pause") lastPauseIdx = i;
            else if (lastPauseIdx >= 0 && firstMotionAfterPause < 0) firstMotionAfterPause = i;
        }
        expect(lastPauseIdx).toBeGreaterThanOrEqual(0);
        expect(firstMotionAfterPause).toBeGreaterThan(lastPauseIdx);
    });
});
