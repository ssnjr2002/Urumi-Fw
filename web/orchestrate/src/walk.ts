/**
 * walk.ts — runtime orchestrator walk.
 *
 * Drives a Schedule over the actual Plan, emitting the MicroSegments that
 * fill the gaps between blocks: inter-block travel jogs, A-home / revolver
 * slot selection, head-offset jogs on head switches, and pause markers where
 * the operator must swap tools.
 *
 * The walk is stateful (it tracks posX/posY/aPhys/headIndex) but side-effect
 * free: it returns a WalkEvent[] rather than streaming to hardware. The caller
 * feeds motion events to the RS485 streamer and acts on pause events (prompting
 * the operator, updating the physical mount table, then resuming).
 *
 * Key invariant — A-home before every block:
 *   Compiled blocks have their preOrient segment baked assuming aPhys = 0 at
 *   block entry. For tangential tools this means the preOrient rotates to the
 *   absolute entry tangent from 0, not from whatever A happens to be. The walk
 *   therefore homes A to 0 before every tangential block and before every
 *   revolver block (the revolver then jogs to the target slot from 0).
 *   This guarantees block segments play back correctly regardless of history.
 *
 * Head assignment:
 *   Which physical head socket holds a given tool is runtime state — it is NOT
 *   encoded in the Schedule (which only knows tool types). The caller provides
 *   a `headAssignment` map (ToolType → headIndex). For a single-head machine
 *   every tool maps to head 0 (the default when omitted).
 */

import type { MachineConfig, ResolvedAxes, ToolType } from "../../config/config.js";
import type { Plan } from "../../plan/src/plan.js";
import type { MicroSegment } from "../../wire/src/microsegment.js";
import type { Schedule, MountSet } from "./schedule.js";
import {
    aMoveTo,
    travelJog,
    headOffsetJog,
} from "../../choreograph/src/choreograph.js";

// ── types ─────────────────────────────────────────────────────────────────────

/** One unit of output from the walk. */
export type WalkEvent =
    | { readonly kind: "motion"; readonly segments: readonly MicroSegment[] }
    | { readonly kind: "pause"; readonly swapIn: MountSet; readonly swapOut: MountSet };

/** Mutable state the walk maintains across blocks. All positions in TRUE steps (pre-invert). */
export interface WalkState {
    posX: number;
    posY: number;
    /** Physical A position in TRUE steps from the last A-home (0 = homed). */
    aPhys: number;
    /** Index into machine.heads of the currently active head. */
    headIndex: number;
}

export interface WalkOptions {
    /**
     * Maps each ToolType in the plan to the head index that holds it.
     * Absent entries default to head 0. For a single-head machine, omit.
     */
    readonly headAssignment?: ReadonlyMap<ToolType, number>;
    /**
     * Minimum feed velocity (mm/s) for travel jog interval clamping.
     * Default 0.5 (matches default QualityConfig.vMin).
     */
    readonly vMin?: number;
    /** Override jog feed (mm/s). Defaults to machine.jogFeed. */
    readonly jogFeed?: number;
    /** Initial machine state. Defaults to origin, A=0, head 0. */
    readonly initialState?: Partial<WalkState>;
}

// ── helpers ───────────────────────────────────────────────────────────────────

function axesForHead(machine: MachineConfig, headIndex: number): ResolvedAxes {
    const head = machine.heads[Math.min(headIndex, machine.heads.length - 1)]!;
    return { x: machine.x, y: machine.y, z: head.z, a: head.a, fCpu: machine.fCpu };
}

/**
 * Accumulate the net XY and A displacement of a segment list (un-applying
 * axis invert so the result is in TRUE step space).
 */
function netDisplacement(
    segs: readonly MicroSegment[],
    axes: ResolvedAxes,
): { dx: number; dy: number; da: number } {
    let dx = 0, dy = 0, da = 0;
    for (const s of segs) {
        dx += axes.x.invert ? -s.dx : s.dx;
        dy += axes.y.invert ? -s.dy : s.dy;
        da += axes.a.invert ? -s.da : s.da;
    }
    return { dx, dy, da };
}

// ── walk ──────────────────────────────────────────────────────────────────────

/**
 * Walk a Schedule, emitting WalkEvents in execution order.
 *
 * Each motion event is a flat MicroSegment[] ready to stream. Each pause event
 * tells the caller which tools to swap before continuing. The caller drives
 * the loop: stream motion, act on pauses, then call this function's generator
 * (or buffer the full output and advance accordingly).
 */
export function walkSchedule(
    schedule: Schedule,
    plan: Plan,
    machine: MachineConfig,
    opts: WalkOptions = {},
): WalkEvent[] {
    const {
        headAssignment = new Map(),
        vMin = 0.5,
    } = opts;

    const state: WalkState = {
        posX: 0,
        posY: 0,
        aPhys: 0,
        headIndex: 0,
        ...opts.initialState,
    };

    const events: WalkEvent[] = [];

    function push(segs: MicroSegment[]) {
        if (segs.length > 0) events.push({ kind: "motion", segments: segs });
    }

    function aHome(axes: ResolvedAxes): void {
        if (state.aPhys === 0) return;
        const { segments, newAPhys } = aMoveTo(0, state.aPhys, axes);
        push(segments);
        state.aPhys = newAPhys;
    }

    for (const phase of schedule.phases) {
        // ── phase boundary: A-home then pause if there's a swap ──────────────
        if (phase.swapIn.length > 0 || phase.swapOut.length > 0) {
            const axes = axesForHead(machine, state.headIndex);
            aHome(axes);
            events.push({ kind: "pause", swapIn: phase.swapIn, swapOut: phase.swapOut });
        }

        // ── execute blocks in this phase ──────────────────────────────────────
        for (const blockIdx of phase.blockIndices) {
            const block = plan.blocks[blockIdx]!;
            const targetHead = headAssignment.get(block.profile.toolType) ?? 0;
            const prevAxes = axesForHead(machine, state.headIndex);
            const axes = axesForHead(machine, targetHead);
            const jogFeed = opts.jogFeed ?? machine.jogFeed;

            const interBlock: MicroSegment[] = [];

            // ── head switch ───────────────────────────────────────────────────
            if (targetHead !== state.headIndex) {
                // home A on the old head before switching
                if (state.aPhys !== 0) {
                    const { segments, newAPhys } = aMoveTo(0, state.aPhys, prevAxes);
                    interBlock.push(...segments);
                    state.aPhys = newAPhys;
                }
                const jog = headOffsetJog(
                    machine.heads[state.headIndex]!,
                    machine.heads[targetHead]!,
                    axes,
                    vMin,
                    jogFeed,
                );
                if (jog) interBlock.push(jog);
                state.headIndex = targetHead;
            }

            // ── A management ──────────────────────────────────────────────────
            if (block.profile.tangential) {
                // Compiled preOrient assumes aPhys=0 at block entry — home A.
                if (state.aPhys !== 0) {
                    const { segments, newAPhys } = aMoveTo(0, state.aPhys, axes);
                    interBlock.push(...segments);
                    state.aPhys = newAPhys;
                }
            } else if (block.slot !== undefined) {
                // Revolver: home A first, then rotate to the target slot.
                if (state.aPhys !== 0) {
                    const { segments, newAPhys } = aMoveTo(0, state.aPhys, axes);
                    interBlock.push(...segments);
                    state.aPhys = newAPhys;
                }
                const slotDeg = block.profile.slotOffsets?.[block.slot] ?? 0;
                if (slotDeg !== 0) {
                    const { segments, newAPhys } = aMoveTo(slotDeg, state.aPhys, axes);
                    interBlock.push(...segments);
                    state.aPhys = newAPhys;
                }
            }

            // ── travel jog to block start ─────────────────────────────────────
            const start = block.startSteps ?? { x: 0, y: 0 };
            const jog = travelJog(state.posX, state.posY, start.x, start.y, axes, vMin, jogFeed);
            if (jog) interBlock.push(jog);
            state.posX = start.x;
            state.posY = start.y;

            push(interBlock);

            // ── block segments ────────────────────────────────────────────────
            push([...block.segments]);

            // Update state from the block's net displacement.
            const { dx, dy, da } = netDisplacement(block.segments, axes);
            state.posX += dx;
            state.posY += dy;
            state.aPhys += da;
        }
    }

    return events;
}
