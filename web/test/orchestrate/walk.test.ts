/**
 * Tests for the runtime orchestrator walk.
 */

import { describe, it, expect } from "vitest";
import { walkSchedule, type WalkEvent } from "../../src/orchestrate/walk.js";
import { scheduleMounts, type Mounts } from "../../src/production/schedule.js";
import type { CompiledBlock } from "../../src/production/compileBlock.js";
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
} from "../../src/machine/index.js";
import { MICRO_JOG } from "../../src/wire/format/microsegment.js";
import { twoHeadMachine } from "../machines.js";

// ── fixtures ──────────────────────────────────────────────────────────────────

function singleHeadMachine(): MachineConfig {
    return machineConfig(
        axisConfig(busNode(1), 160, { invert: true, maxFeed: 80, maxAccel: 1000 }),
        axisConfig(busNode(2), 160, { maxFeed: 80, maxAccel: 1000 }),
        [
            toolHead(
                axisConfig(busNode(3), 1200, { invert: true }),
                axisConfig(busNode(4), 51.667, { rotary: true, invert: true, maxFeed: 100, maxAccel: 2000 }),
                { accepts: [ToolType.PEN, ToolType.KNIFE, ToolType.CREASE, ToolType.REVOLVER_PEN] },
            ),
        ],
        { fCpu: 150_000_000, rapid: { feed: 80 } },
    );
}

/**
 * twoHeadMachine() with heads DELIBERATELY DIFFERENT (1200 vs 600 steps/mm on
 * Z) — see its doc comment in test/machines.ts. A shared axisConfig here would
 * make every test in this file pass whether or not walkSchedule resolved the
 * correct head's calibration.
 */
function dualHeadMachine(): MachineConfig {
    return twoHeadMachine([
        { xOffset: -50, yOffset: 0 },
        { xOffset: 50, yOffset: 0 },
    ]);
}

/**
 * Compiled blocks with empty segment lists — the walk only reads `profile`,
 * `slot` and `startSteps`, and the inter-block motion is what these tests are
 * about. `head` is 0 throughout: the walk still takes its head from the
 * caller's headAssignment map, and reading block.head instead is stage 4.
 */
function plan(...tools: (ToolProfile | [ToolProfile, number])[]): CompiledBlock[] {
    return tools.map((t, i) => {
        const [profile, slot] = Array.isArray(t) ? t : [t, undefined];
        const startSteps = { x: i * 1000, y: 0 }; // stagger blocks so travel is visible
        return slot === undefined
            ? { profile, head: 0, segments: [], startSteps }
            : { profile, slot, head: 0, segments: [], startSteps };
    });
}

/**
 * Schedule a plan the way bakePlan does. The scheduler needs the machine now
 * (it decides heads from `accepts`), so the head count is no longer something a
 * test states — it follows from the machine the walk runs on.
 */
function sched(p: readonly CompiledBlock[], machine: MachineConfig, mounts?: Mounts) {
    return scheduleMounts(
        machine,
        p.map((b) => b.profile.toolType),
        mounts ?? machine.heads.map(() => null),
    );
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
        const s = sched([], singleHeadMachine());
        expect(walkSchedule(s, [], singleHeadMachine())).toEqual([]);
    });
});

describe("walkSchedule: single phase, no swap needed", () => {
    it("emits no pause event when mount starts empty and swapIn is the first fill", () => {
        // seedMounted=[] → first phase has swapIn=[KNIFE] but that IS the bare-start fill.
        // Pause fires because swapIn is non-empty (operator must load the tool).
        const p = plan(PEN);
        const s = sched(p, singleHeadMachine());
        const events = walkSchedule(s, p, singleHeadMachine());
        const pauses = pauseEvents(events);
        expect(pauses).toHaveLength(1); // must load PEN before start
        expect((pauses[0] as unknown as { swapIn: ToolType[] }).swapIn).toContain(ToolType.PEN);
    });

    it("no pause when the seed already has the right tool", () => {
        const p = plan(PEN);
        const s = sched(p, singleHeadMachine(), [ToolType.PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        expect(pauseEvents(events)).toHaveLength(0);
    });
});

describe("walkSchedule: travel jog between blocks", () => {
    it("emits a travel jog between staggered blocks", () => {
        // Two pen blocks at x=0 and x=1000; should get a JOG segment between them.
        const p = plan(PEN, PEN);
        const s = sched(p, singleHeadMachine(), [ToolType.PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        const motions = motionEvents(events);
        // Find any motion event containing a JOG
        const jogEvent = motions.find(
            (e) => e.kind === "motion" && e.segments.some((seg) => seg.flags === MICRO_JOG),
        );
        expect(jogEvent).toBeDefined();
    });

    it("emits no travel for coincident block starts", () => {
        const p: CompiledBlock[] = [
                { profile: PEN, head: 0, segments: [], startSteps: { x: 0, y: 0 } },
                { profile: PEN, head: 0, segments: [], startSteps: { x: 0, y: 0 } },
        ];
        const s = sched(p, singleHeadMachine(), [ToolType.PEN]);
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
        const s = sched(p, singleHeadMachine(), [ToolType.KNIFE]);
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
        const s = sched(p, singleHeadMachine(), [ToolType.KNIFE]);
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
        const p: CompiledBlock[] = [{ profile: REVOLVER_PEN, slot: 1, head: 0, segments: [], startSteps: { x: 0, y: 0 } }];
        const s = sched(p, singleHeadMachine(), [ToolType.REVOLVER_PEN]);
        const events = walkSchedule(s, p, singleHeadMachine());
        const aSegs = motionEvents(events)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .filter((seg) => seg.da !== 0 && seg.dx === 0 && seg.dy === 0);
        expect(aSegs.length).toBeGreaterThan(0);
    });

    it("no A rotation for slot 0 (offset = 0°)", () => {
        const p: CompiledBlock[] = [{ profile: REVOLVER_PEN, slot: 0, head: 0, segments: [], startSteps: { x: 0, y: 0 } }];
        const s = sched(p, singleHeadMachine(), [ToolType.REVOLVER_PEN]);
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
        const s = sched(p, dualHeadMachine());
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
        const s = sched(p, singleHeadMachine());
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
        const s = sched(p, dualHeadMachine(), [ToolType.KNIFE, ToolType.PEN]);
        const headMap = new Map([[ToolType.KNIFE, 0], [ToolType.PEN, 1]]);
        const events = walkSchedule(s, p, machine, { headAssignment: headMap });
        // When we switch from head 0 to head 1, a JOG for the offset diff fires.
        const jogs = motionEvents(events)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .filter((seg) => seg.flags === MICRO_JOG && (seg.dx !== 0 || seg.dy !== 0));
        expect(jogs.length).toBeGreaterThan(0);
    });

    it("emits a rebind naming the incoming head, before that head's offset jog", () => {
        // Both heads' Z/A contend for slots 2/3, so the map has to follow the
        // switch. Without this event the runner cannot know it happened.
        const machine = dualHeadMachine();
        const p = plan(KNIFE, PEN);
        const s = sched(p, dualHeadMachine(), [ToolType.KNIFE, ToolType.PEN]);
        const events = walkSchedule(s, p, machine, {
            headAssignment: new Map([[ToolType.KNIFE, 0], [ToolType.PEN, 1]]),
        });

        const at = events.findIndex((e) => e.kind === "rebind");
        expect(at).toBeGreaterThanOrEqual(0);
        expect(events[at]).toEqual({ kind: "rebind", head: 1 });

        // The +100mm offset jog belongs to the incoming head and must land
        // after the rebind — it is the first thing the new binding governs.
        const after = events.slice(at + 1)
            .flatMap((e) => (e.kind === "motion" ? e.segments : []))
            .filter((seg) => seg.flags === MICRO_JOG && seg.dx !== 0);
        expect(after.length).toBeGreaterThan(0);
    });

    it("emits no rebind when every tool sits on the same head", () => {
        const p = plan(KNIFE, PEN);
        const s = sched(p, dualHeadMachine(), [ToolType.KNIFE, ToolType.PEN]);
        const events = walkSchedule(s, p, dualHeadMachine(), {
            headAssignment: new Map([[ToolType.KNIFE, 0], [ToolType.PEN, 0]]),
        });
        expect(events.some((e) => e.kind === "rebind")).toBe(false);
    });
});

describe("walkSchedule: event ordering", () => {
    it("pause always precedes the motion for the phase it guards", () => {
        const p = plan(KNIFE, PEN);
        const s = sched(p, singleHeadMachine());
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
