/**
 * production/schedule.ts — which tool sits in which socket, and where the
 * operator has to intervene.
 *
 * A machine has a fixed number of heads. A job demands a tool per block. When
 * it needs more tools than there are sockets, it cannot run in one pass: the
 * operator loads some tools, runs every block those tools can cut, then pauses
 * to swap and continues. This module computes that batching offline.
 *
 * The loop (greedy, document order — never reorders blocks):
 *   1. FILL    — seat the upcoming blocks' tools into free sockets.
 *   2. EXECUTE — run blocks in order while their tool is seated.
 *   3. PAUSE   — stop at the first block whose tool is not.
 *   4. SWAP    — that block opens the next phase; refill.
 *
 * It also DECIDES THE HEAD. `SwapPhase.mounts` is head-indexed, so a block's
 * head is the index holding its tool. There is no separate assignHeads: this is
 * the only layer that must reason about socket contention, so it is the layer
 * that resolves it, and assignment falls out as an output. Everything
 * downstream reads that answer rather than re-deriving it — see
 * docs/head_binding.md.
 *
 * The REVOLVER_PEN is one tool occupying one head regardless of how many slots
 * it serves — its blocks differ only by `slot`, share a toolType, and so stay
 * in one phase (slot changes are intra-phase A rotations, not swaps).
 *
 * Pure scheduling: no physical state, no motion, no geometry. It reads tool
 * types and `heads[].accepts` and nothing else.
 */

import type { MachineConfig, ToolType } from "../machine/index.js";
import { headsAccepting } from "../machine/resolve.js";
import { TOOL_PROFILES_BY_TYPE } from "../machine/tools.js";

/**
 * What sits in each socket, by head index. null = empty.
 *
 * Head-indexed, not a set: `[KNIFE, PEN]` and `[PEN, KNIFE]` are different
 * arrangements of the same tools, and the difference is exactly whether an
 * operator has to move anything.
 */
export type Mounts = readonly (ToolType | null)[];

/** One fill→execute batch: what is mounted, and the blocks that run under it. */
export interface SwapPhase {
    /** The mount table in force for this phase. A block's head is its index here. */
    readonly mounts: Mounts;
    /** Block indices run in this phase, in execution order. */
    readonly blockIndices: readonly number[];
    /** Tools to fit at the pause before this phase. */
    readonly swapIn: readonly ToolType[];
    /** Tools to remove at the pause before this phase. */
    readonly swapOut: readonly ToolType[];
}

/** (added, removed) going from `prev` to `next`, ignoring which socket. */
export function mountDiff(
    prev: Mounts,
    next: Mounts,
): { added: ToolType[]; removed: ToolType[] } {
    const has = (m: Mounts, t: ToolType) => m.includes(t);
    const present = (m: Mounts) => m.filter((t): t is ToolType => t !== null);
    return {
        added: present(next).filter((t) => !has(prev, t)),
        removed: present(prev).filter((t) => !has(next, t)),
    };
}

/**
 * Reject a mount table that cannot physically exist on this machine.
 *
 * The caller supplies this table, and a wrong one does not fail loudly on its
 * own — it just produces a schedule that seats tools where they do not fit.
 * Naming it here beats discovering it at the machine.
 */
export function validateMounts(machine: MachineConfig, mounts: Mounts): void {
    if (mounts.length !== machine.heads.length) {
        throw new Error(
            `mounts has ${mounts.length} entries but the machine has ` +
                `${machine.heads.length} head(s)`,
        );
    }
    mounts.forEach((t, i) => {
        if (t === null) return;
        if (!machine.heads[i]!.accepts.includes(t)) {
            const name = TOOL_PROFILES_BY_TYPE[t]?.name ?? `tool ${t}`;
            throw new Error(`head ${i} does not accept '${name}'`);
        }
    });
}

/**
 * Seat this exact set of tools, one per socket, or return null if it does not
 * fit at all.
 *
 * This is a bipartite matching, not a greedy placement, and it has to be: a
 * tool that fits several heads can occupy the only socket another tool has,
 * and greedy placement cannot take that back. Kuhn's algorithm can — when a
 * head is taken it asks the current occupant to move, recursively.
 *
 * Greedy placed in most-constrained-first order gets the common cases right and
 * is still wrong in general; matching is exact, order-invariant by
 * construction, and about the same amount of code. The sets are at most one
 * tool per head, so the cost is irrelevant.
 *
 * `current` steers it without constraining it: each tool tries the socket it is
 * already in first, so an arrangement that needs no operator work is the one
 * found, and a tool stays put across phases (and across jobs, when the caller
 * passes the live table) unless something has to move.
 */
function seat(
    machine: MachineConfig,
    tools: readonly ToolType[],
    current: Mounts,
): Mounts | null {
    const occupant: (ToolType | null)[] = machine.heads.map(() => null);

    const options = (t: ToolType): number[] => {
        const heads = headsAccepting(machine, t);
        const held = current.indexOf(t);
        // Where it already sits goes first — that is the carry-forward.
        return held >= 0 && heads.includes(held)
            ? [held, ...heads.filter((h) => h !== held)]
            : heads;
    };

    const place = (t: ToolType, tried: Set<number>): boolean => {
        for (const h of options(t)) {
            if (tried.has(h)) continue;
            tried.add(h);
            const sitting = occupant[h];
            if (sitting === null || sitting === undefined || place(sitting, tried)) {
                occupant[h] = t;
                return true;
            }
        }
        return false;
    };

    for (const t of tools) {
        if (!place(t, new Set())) return null;
    }
    return occupant;
}

/**
 * How many of the upcoming blocks' tools can be mounted at once, and where.
 *
 * Grows the set in FIRST-USE order and stops at the first tool that will not
 * fit alongside the others. First-use order is the right order here even though
 * placement must not depend on it: execution stops at the first block whose
 * tool is missing, so a tool first needed after that point cannot run this
 * phase however well it would have fitted.
 *
 * Which tools share a phase therefore still follows block order — a known limit
 * that cannot go away without reordering blocks. Where they SIT does not: that
 * is `seat`'s matching over the resulting set.
 */
function fillFrom(
    machine: MachineConfig,
    tools: readonly ToolType[],
    start: number,
    current: Mounts,
): Mounts {
    const chosen: ToolType[] = [];
    // A single tool always seats (every tool has an accepting head, checked up
    // front), so this is never null by the time it is returned.
    let best: Mounts = machine.heads.map(() => null);

    for (let j = start; j < tools.length; j++) {
        const t = tools[j]!;
        if (chosen.includes(t)) continue;
        const trial = seat(machine, [...chosen, t], current);
        if (trial === null) break;
        chosen.push(t);
        best = trial;
    }
    return best;
}

/**
 * Batch a job's tool sequence into swap phases, assigning heads as it goes.
 *
 * `tools` is one entry per block, in execution order (which is document order —
 * nothing reorders blocks; see order.ts).
 *
 * `mounts` is what is in the sockets NOW, and this function does not care where
 * that came from. `[null, null]` is a bare machine and gives a reproducible
 * bake; a live `Setup`'s table gives the fewest operator swaps. Neither is
 * hidden state, and the choice is the caller's:
 *
 *     scheduleMounts(machine, tools, [null, null])          // reproducible
 *     scheduleMounts(machine, tools, mountedTypes(setup))   // swap-minimal
 *
 * The distinction matters because the head decides step counts: a plan baked
 * against a live rig is not byte-identical to one baked against a bare machine.
 *
 * Throws on a mount table this machine cannot hold, and on a tool no head
 * accepts — both are config or caller errors that would otherwise surface as a
 * job that mysteriously never schedules a block.
 */
export function scheduleMounts(
    machine: MachineConfig,
    tools: readonly ToolType[],
    mounts: Mounts,
): readonly SwapPhase[] {
    validateMounts(machine, mounts);

    for (const t of new Set(tools)) {
        if (headsAccepting(machine, t).length === 0) {
            const name = TOOL_PROFILES_BY_TYPE[t]?.name ?? `tool ${t}`;
            throw new Error(`no head accepts '${name}' — this job cannot run on this machine`);
        }
    }

    const phases: SwapPhase[] = [];
    let current: Mounts = mounts;
    let i = 0;

    while (i < tools.length) {
        const filled = fillFrom(machine, tools, i, current);

        const blockIndices: number[] = [];
        while (i < tools.length && filled.includes(tools[i]!)) {
            blockIndices.push(i);
            i++;
        }

        // A fill that seats nothing the next block needs would loop forever.
        // It cannot happen — every tool has an accepting head, and the fill
        // frees unwanted sockets before seating — so this is an assertion, not
        // a recovery path.
        if (blockIndices.length === 0) {
            throw new Error(
                `scheduler made no progress at block ${i} — could not seat its tool`,
            );
        }

        const { added, removed } = mountDiff(current, filled);
        phases.push({ mounts: filled, blockIndices, swapIn: added, swapOut: removed });
        current = filled;
    }

    return phases;
}
