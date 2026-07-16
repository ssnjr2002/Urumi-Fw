/**
 * helpers.ts — config resolution/query helpers.
 *
 * Policy on top of the config *data* in config.ts: mapping an SVG layer name
 * to the tool that cuts it, and the config-only feasibility gate. Kept out of
 * config.ts so that file stays pure type definitions + factories.
 * 
 * TODO: Explore debloating config.ts further by moving even more helpers out
 * of there to here.
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
 * Which axes a tool's motion actually drives, beyond the always-present X/Y:
 *   - Z when the tool lifts (liftHeight > 0) — a tool with no lift emits no
 *     Z moves, so it does not require a Z node.
 *   - A when the tool steers it: tangential tools track the path tangent;
 *     the revolver selects slots by A rotation (slotOffsets present).
 *
 * Keyed on behaviour, not tool identity, so "required iff actually used".
 */
export function requiredAxes(profile: ToolProfile): { readonly z: boolean; readonly a: boolean } {
    return {
        z: profile.liftHeight > 0,
        a: profile.tangential || profile.slotOffsets !== undefined,
    };
}

/**
 * (ok, reason) — is `profile` PHYSICALLY runnable on this machine's bus? The
 * bake-time feasibility gate. NOT a mount check (which tool is screwed in is
 * runtime state); this asks whether the nodes the tool needs are wired up
 * (`present`) on the bus:
 *   - X and Y axis nodes (always),
 *   - a Z node on some head (if the tool lifts),
 *   - an A node on some head (if the tool steers A),
 *   - a present peripheral for each requiredPeripheralTypes entry.
 *
 * `present` means attached/wired, not alive — no ping is issued. reason lists
 * the missing nodes when ok is false.
 */
export function canRunTool(
    machine: MachineConfig,
    profile: ToolProfile,
): readonly [boolean, string] {
    const missing: string[] = [];

    if (!machine.x.node.present) missing.push("X axis node");
    if (!machine.y.node.present) missing.push("Y axis node");

    const req = requiredAxes(profile);
    if (req.z && !machine.heads.some((h) => h.z.node.present)) missing.push("Z axis node");
    if (req.a && !machine.heads.some((h) => h.a.node.present)) missing.push("A axis node");

    for (const type of profile.requiredPeripheralTypes) {
        if (!machine.peripherals.some((p) => p.present && p.type === type)) {
            missing.push(`peripheral type 0x${type.toString(16).padStart(2, "0")}`);
        }
    }

    if (missing.length > 0) {
        return [false, `${profile.name}: bus is missing ${missing.join(", ")}`];
    }
    return [true, ""];
}
