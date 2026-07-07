/**
 * helpers.ts — config resolution/query helpers.
 *
 * Policy on top of the config *data* in config.ts: mapping an SVG layer name
 * to the tool that cuts it, and the config-only feasibility gate. Kept out of
 * config.ts so that file stays pure type definitions + factories.
 */

import type { MachineConfig, ToolProfile } from "./config.js";
import { TOOL_PROFILES } from "./config.js";

/**
 * Resolve an SVG layer name to the tool that cuts it. Case-insensitive match
 * against the profile registry's keys (a layer named "Knife" picks KNIFE).
 * Returns undefined for an unrecognised name — the caller decides whether to
 * fall back to a default tool or reject the layer.
 *
 * `name` is a single layer label, NOT a '/'-separated key: revolver slot
 * parsing (splitting "revolver_pen/slot3" into parent + slot) is the block
 * assembler's job. This only maps one label to one profile.
 */
export function toolForLayer(
    name: string,
    profiles: Readonly<Record<string, ToolProfile>> = TOOL_PROFILES,
): ToolProfile | undefined {
    const key = name.trim().toLowerCase();
    return profiles[key];
}

/**
 * (ok, reason) — can this machine's topology run `profile`? The config-only
 * feasibility gate: a tool is runnable when some head has that tool type
 * mounted. reason is a human-readable string when ok is false.
 */
export function canRunTool(
    machine: MachineConfig,
    profile: ToolProfile,
): readonly [boolean, string] {
    const mounted = machine.heads.some((h) => h.profile.toolType === profile.toolType);
    if (!mounted) {
        return [false, `no head has a ${profile.name} (type 0x${profile.toolType.toString(16)}) mounted`];
    }
    return [true, ""];
}
