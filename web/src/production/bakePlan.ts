/**
 * bakePlan.ts — config + SVG text → compiled blocks + the phases that run them.
 *
 * The clean top-level bake: no orchestrate/planner layers (unlike Python).
 * We just walk the SVG's layers in document order and assemble one Block per
 * layer, resolving each layer name to its tool. The revolver pen is the one
 * nested case — its slot sub-layers ("revolver_pen/slot3") each become a Block
 * tagged with the slot index.
 *
 *   loadSvgMmLayers → assembleBlocks → orderBlocks → scheduleMounts
 *                   → compileBlock per block → Plan + SwapPhase[]
 *
 * Ordering and scheduling sit BEFORE the compile because the head decides step
 * counts (docs/head_binding.md): Z/A stepsPerUnit, invert and the feed/accel
 * ceilings are all per-head, and they shape the trajectory rather than scaling
 * it, so mm cannot become steps until the head is known.
 *
 * All machine/tool/quality options come from the parsed PipelineConfig (the
 * config.json path via loadConfig). The only genuinely job-level choice is a
 * fallback tool for an unlayered SVG.
 */

import type { CubicBezier } from "../toolpath/geometry.js";
import type { PipelineConfig, ToolProfile } from "../machine/index.js";
import { ToolType } from "../machine/index.js";
import { toolForLayer } from "../machine/resolve.js";
import { loadSvgMmLayers, loadSvgLayers } from "../svg/ingest.js";
import { compileBlock, type Block } from "./compileBlock.js";
import { orderBlocks } from "./order.js";
import { scheduleMounts, type Mounts, type SwapPhase } from "./schedule.js";
import { setupFor, mountedTypes } from "../machine/setup.js";
import type { Block as CompiledPlanBlock, Plan } from "../plan/plan.js";

export interface BakePlanOptions {
    /** Fallback tool name for an unlayered SVG (the '' layer). */
    readonly defaultTool?: string;
    readonly skipNormalisation?: boolean;
    /**
     * What is in the sockets NOW, for the scheduler to start from. Defaults to
     * the config's own preferred arrangement (`setupFor`), which makes a bake
     * reproducible from the config alone. Pass a live `mountedTypes(setup)` to
     * bake for the fewest operator swaps instead — see production/schedule.ts.
     */
    readonly mounts?: Mounts;
}

const SLOT_RE = /^slot(\d+)$/i;

/**
 * Parse a revolver slot sub-layer name ("slot1".."slotN") to a 0-based index.
 * slot1 → index 0. Throws on a malformed name or an out-of-range slot.
 */
function parseSlot(child: string, profile: ToolProfile): number {
    const m = SLOT_RE.exec(child.trim());
    if (!m) {
        throw new Error(
            `revolver layer child '${child}' is not a slot (expected 'slot1'..'slotN')`,
        );
    }
    const slot = Number(m[1]) - 1; // slot1 → index 0
    const count = profile.slotOffsets?.length ?? 0;
    if (slot < 0 || slot >= count) {
        throw new Error(`slot ${m[1]} out of range (${profile.name} has ${count} slots)`);
    }
    return slot;
}

/**
 * Resolve the SVG's layers to an ordered list of tool-tagged geometry blocks.
 * Document order is preserved (loadSvgMmLayers yields layers in order).
 *
 * Resolution per layer key:
 *   - "" (unlayered)         → opts.defaultTool, or throw
 *   - "knife" / "pen" / …    → that tool, no slot
 *   - "revolver_pen/slotN"   → REVOLVER_PEN, slot N-1
 */
export function assembleBlocks(
    layers: Map<string, CubicBezier[][]>,
    config: PipelineConfig,
    opts: BakePlanOptions = {},
): Block[] {
    const profiles = config.toolProfiles;
    const blocks: Block[] = [];

    for (const [key, subpaths] of layers) {
        const parts = key.split("/");
        const head = parts[0] ?? "";

        // unlayered SVG: single '' layer → default tool
        if (head === "") {
            const profile = opts.defaultTool ? toolForLayer(opts.defaultTool, profiles) : undefined;
            if (!profile) {
                throw new Error(
                    "unlayered SVG — pass defaultTool (a tool name) to bake it as a one-tool job",
                );
            }
            blocks.push({ profile, subpaths });
            continue;
        }

        const profile = toolForLayer(head, profiles);
        if (!profile) {
            throw new Error(
                `layer '${key}' has no tool — rename it to a tool (pen/knife/crease/` +
                    `revolver_pen) or set defaultTool`,
            );
        }

        if (profile.toolType === ToolType.REVOLVER_PEN) {
            const child = parts[1];
            if (child === undefined) {
                throw new Error(
                    `revolver layer '${key}' needs slot sub-layers (e.g. '${key}/slot1')`,
                );
            }
            blocks.push({ profile, slot: parseSlot(child, profile), subpaths });
        } else {
            blocks.push({ profile, subpaths });
        }
    }

    return blocks;
}

/**
 * SVG text + config → compiled blocks and the phases that run them.
 *
 * Order and scheduling now happen HERE, before anything is compiled, because
 * the head decides step counts and only the scheduler knows the head. The
 * phases come back alongside the plan rather than being recomputed downstream:
 * a walk that re-derived them could disagree with what was baked.
 *
 * `bytes` is gone. A serialised .plan encodes step counts resolved against one
 * head arrangement, so a file baked under one `accepts` config is silently
 * wrong under another — see docs/head_binding.md. Callers that still want a
 * file call savePlan() themselves, for as long as that survives.
 */
export function bakePlan(
    config: PipelineConfig,
    svgText: string,
    opts: BakePlanOptions = {},
): { plan: Plan; phases: readonly SwapPhase[] } {
    const layers = opts.skipNormalisation
        ? loadSvgLayers(svgText)
        : loadSvgMmLayers(svgText).layers;

    const ordered = orderBlocks(assembleBlocks(layers, config, opts), config.machine);
    const phases = scheduleMounts(
        config.machine,
        ordered.map((b) => b.profile.toolType),
        opts.mounts ?? mountedTypes(setupFor(config.machine)),
    );

    const blocks: CompiledPlanBlock[] = ordered.map((b) => {
        const { segments, startSteps } = compileBlock(
            b.subpaths, config.machine, config.quality, b.profile,
        );
        return b.slot === undefined
            ? { profile: b.profile, segments, startSteps }
            : { profile: b.profile, slot: b.slot, segments, startSteps };
    });

    return { plan: { blocks }, phases };
}
