/**
 * bakePlan.ts — config + SVG text → a Plan (and its .plan bytes).
 *
 * The clean top-level bake: no orchestrate/planner layers (unlike Python).
 * We just walk the SVG's layers in document order and assemble one Block per
 * layer, resolving each layer name to its tool. The revolver pen is the one
 * nested case — its slot sub-layers ("revolver_pen/slot3") each become a Block
 * tagged with the slot index.
 *
 *   loadSvgMmLayers → assembleBlocks → compileBlock per block → Plan → savePlan
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
import { compileBlock } from "./compileBlock.js";
import type { Block, Plan } from "../plan/plan.js";
import { savePlan } from "../plan/planFile.js";

export interface BakePlanOptions {
    /** Fallback tool name for an unlayered SVG (the '' layer). */
    readonly defaultTool?: string;
    readonly skipNormalisation?: boolean;
}

/** One layer resolved to its tool + slot, still as geometry (pre-compile). */
interface LayerBlock {
    readonly profile: ToolProfile;
    readonly slot?: number;
    readonly subpaths: readonly (readonly CubicBezier[])[];
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
): LayerBlock[] {
    const profiles = config.toolProfiles;
    const blocks: LayerBlock[] = [];

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
 * SVG text + config → a Plan and its serialised .plan bytes.
 *
 * Walks the SVG's layers, compiles each block through the tool-aware pipeline
 * (compileBlock), and serialises the result. `bytes` is a ready-to-write
 * .plan file; `plan` is the in-memory model (for inspection or streaming).
 */
export function bakePlan(
    config: PipelineConfig,
    svgText: string,
    opts: BakePlanOptions = {},
): { plan: Plan; bytes: Uint8Array } {
    const layers = opts.skipNormalisation
        ? loadSvgLayers(svgText)
        : loadSvgMmLayers(svgText).layers;
    const layerBlocks = assembleBlocks(layers, config, opts);

    const blocks: Block[] = layerBlocks.map((lb) => {
        const { segments, startSteps } = compileBlock(
            lb.subpaths, config.machine, config.quality, lb.profile,
        );
        return lb.slot === undefined
            ? { profile: lb.profile, segments, startSteps }
            : { profile: lb.profile, slot: lb.slot, segments, startSteps };
    });

    const plan: Plan = { blocks };
    return { plan, bytes: savePlan(plan) };
}
