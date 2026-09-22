/**
 * production/order.ts — execution order, ahead of scheduling.
 *
 * A PLACEHOLDER: returns document order unchanged. The value here is the seam,
 * not the code — ordering policy needs a home upstream of the scheduler, or it
 * grows inside it and tangles with head assignment.
 */

import type { MachineConfig } from "../machine/index.js";
import type { Block } from "./compileBlock.js";

/**
 * Reorder blocks before heads are assigned. Identity for now.
 *
 * Contract: HEAD-INDEPENDENT CONSTRAINTS ONLY — "crease before the cut that
 * frees the part", "keep a tool's blocks contiguous". Reordering to minimise
 * swaps does not belong here: that objective cannot be evaluated without the
 * assignment, which depends on the order, and a joint problem wants its own
 * stage rather than a bigger version of this one.
 */
export function orderBlocks(
    blocks: readonly Block[],
    _machine: MachineConfig,
): readonly Block[] {
    return blocks;
}
