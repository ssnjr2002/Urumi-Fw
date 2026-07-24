/**
 * plan.ts — the Block / Plan job model.
 *
 * A Plan is an ordered list of Blocks: the whole SVG compiled to wire events,
 * one Block per SVG layer (or per revolver slot sub-layer). Deviates from the
 * Python host.production.plan_io.Plan model — the maths is already parity-
 * verified, so this layer is free to be cleaner:
 *
 *   - a Block carries decoded MicroSegment[], not opaque packet bytes (the
 *     .plan codec in planFile.ts serialises them on save, decodes on load,
 *     so save/load round-trips)
 *   - a Block carries an optional `slot` for the revolver pen — the piece the
 *     Python model had no notion of
 *
 * The slot is metadata: the runtime orchestrator jogs A to
 * profile.slotOffsets[slot] before executing the block. It does NOT change the
 * packet maths, so parity with the single-tool bake is untouched.
 */

import type { MachineConfig, ToolProfile, ToolType } from "../config/config.js";
import { canRunTool } from "../config/resolve.js";
import type { MicroSegment } from "../wire/format/microsegment.js";

/** One SVG layer's worth of compiled motion, tagged with its tool + slot. */
export interface Block {
    /** The resolved tool profile that cuts this block. */
    readonly profile: ToolProfile;
    /**
     * Revolver slot index (0-based) when profile is the revolver pen; absent
     * for pen/knife/crease. The orchestrator jogs A to
     * profile.slotOffsets[slot] before this block runs.
     */
    readonly slot?: number;
    /** The compiled wire events for this block, in execution order. */
    readonly segments: readonly MicroSegment[];
    /**
     * Absolute machine position in TRUE steps (pre-invert) of the first
     * subpath's first point, after toolOffset shift. Set by bakePlan.
     *
     * The runtime walk uses this to generate the inter-block travel jog.
     */
    readonly startSteps: { readonly x: number; readonly y: number };
}

/** A whole job: blocks in execution order. */
export interface Plan {
    readonly blocks: readonly Block[];
}

/**
 * Unique tool types the plan uses, in first-appearance order. Backs the
 * upfront tool manifest in the .plan header (feasibility gate before
 * streaming).
 */
export function planToolTypes(plan: Plan): ToolType[] {
    const seen: ToolType[] = [];
    for (const b of plan.blocks) {
        if (!seen.includes(b.profile.toolType)) seen.push(b.profile.toolType);
    }
    return seen;
}

/**
 * (ok, problems) — can `machine`'s bus physically run every tool this plan
 * uses? The upfront gate: for each tool, the nodes it needs are wired
 * (`present`) on the bus — NOT whether the tool is mounted (that's runtime
 * mount-table state). problems lists one (toolName, reason) per tool whose
 * nodes are missing, de-duplicated.
 */
export function feasibleOn(
    plan: Plan,
    machine: MachineConfig,
): readonly [boolean, readonly (readonly [string, string])[]] {
    const problems: (readonly [string, string])[] = [];
    for (const b of plan.blocks) {
        const [ok, reason] = canRunTool(machine, b.profile);
        if (!ok && !problems.some((p) => p[0] === b.profile.name)) {
            problems.push([b.profile.name, reason]);
        }
    }
    return [problems.length === 0, problems];
}
