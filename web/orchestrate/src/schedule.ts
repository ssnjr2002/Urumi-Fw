/**
 * schedule.ts — the fill / execute / pause / swap loop, as a pure planner.
 *
 * A machine has a fixed number of heads (sockets). A Plan demands a tool per
 * block. When the plan uses more distinct tools than there are heads, the job
 * can't run in one pass: the operator loads some tools, runs every block those
 * tools can cut, then PAUSES to swap tools and continues. This module computes
 * that batching offline.
 *
 * The loop (greedy, document order — never reorders blocks):
 *   1. FILL   — from the current block, mount the first `headCount` distinct
 *               tool types the upcoming blocks need.
 *   2. EXECUTE — run blocks in order while their tool is in the mounted set.
 *   3. PAUSE  — stop at the first block whose tool isn't mounted.
 *   4. SWAP   — that block begins the next phase; recompute the mount set.
 * Repeat until every block is scheduled.
 *
 * The REVOLVER_PEN is one tool occupying one head regardless of how many slots
 * it serves — its blocks differ only by `slot`, share a toolType, and so stay
 * in one phase (slot changes are intra-phase A rotations, not swaps).
 *
 * This is pure scheduling: no physical state, no motion. The runtime
 * orchestrator walks a Schedule, reconciles each phase's mount set against the
 * live mount table (emitting operator swap prompts for the diff), and drives
 * the choreograph helpers for the inter-block motion.
 */

import type { Plan } from "../../plan/src/plan.js";
import type { ToolType } from "../../config/config.js";

/** A set of tool types mounted at once. Distinct, length ≤ headCount. */
export type MountSet = readonly ToolType[];

/** One fill→execute batch: the tools mounted and the blocks they run. */
export interface Phase {
    /** Tool types mounted for this phase (distinct, ≤ headCount). */
    readonly mount: MountSet;
    /** plan.blocks indices run in this phase, in execution (document) order. */
    readonly blockIndices: readonly number[];
    /** Tools to mount at the pause before this phase (mount \ previous mount). */
    readonly swapIn: MountSet;
    /** Tools to remove at the pause before this phase (previous mount \ mount). */
    readonly swapOut: MountSet;
}

/** The full batching of a plan onto a machine with `headCount` sockets. */
export interface Schedule {
    readonly headCount: number;
    readonly phases: readonly Phase[];
}

/** (added, removed) tool types going from `prev` to `next` mount sets. */
export function mountDiff(prev: MountSet, next: MountSet): { added: ToolType[]; removed: ToolType[] } {
    const added = next.filter((t) => !prev.includes(t));
    const removed = prev.filter((t) => !next.includes(t));
    return { added, removed };
}

/**
 * The first `headCount` distinct tool types demanded by blocks from `start`
 * onward. Fewer when the tail uses fewer distinct tools.
 */
function fillFrom(plan: Plan, start: number, headCount: number): ToolType[] {
    const mount: ToolType[] = [];
    for (let j = start; j < plan.blocks.length; j++) {
        const t = plan.blocks[j]!.profile.toolType;
        if (!mount.includes(t)) {
            if (mount.length === headCount) break;
            mount.push(t);
        }
    }
    return mount;
}

/**
 * Batch a plan into swap phases for a machine with `headCount` heads.
 *
 * `seedMounted` is the set of tool types already physically loaded (from the
 * live mount table); it only affects the first phase's swapIn/swapOut, not
 * which tools are chosen. Defaults to empty (bare machine).
 *
 * Throws on headCount < 1. An empty plan yields an empty schedule.
 */
export function scheduleMounts(
    plan: Plan,
    headCount: number,
    seedMounted: MountSet = [],
): Schedule {
    if (headCount < 1) {
        throw new Error(`headCount must be >= 1 (got ${headCount})`);
    }

    const phases: Phase[] = [];
    let prevMount: MountSet = seedMounted;
    let i = 0;
    const n = plan.blocks.length;

    while (i < n) {
        const mount = fillFrom(plan, i, headCount);

        // EXECUTE: consume the contiguous run of blocks the mount set can cut.
        const blockIndices: number[] = [];
        while (i < n && mount.includes(plan.blocks[i]!.profile.toolType)) {
            blockIndices.push(i);
            i++;
        }

        const { added, removed } = mountDiff(prevMount, mount);
        phases.push({ mount, blockIndices, swapIn: added, swapOut: removed });
        prevMount = mount;
    }

    return { headCount, phases };
}
