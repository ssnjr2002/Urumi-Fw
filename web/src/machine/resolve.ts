/**
 * resolve.ts — resolution POLICY over the config data.
 *
 * Everything that answers "given this config, what actually applies?":
 *   - resolvedAxes  — which 4 axes are live (Z/A come from the default head)
 *   - resolveTargets — the tool→machine feed/accel override chain
 *   - toolForLayer  — SVG layer name → tool
 *   - requiredAxes / canRunTool — bake-time feasibility
 *
 * Kept out of schema.ts so that file stays types + factories. Stages import
 * from here rather than reimplementing any of these rules inline — the `?? 80`
 * duplication that motivated defaults.ts was exactly that failure.
 */

import type {
    AxisConfig,
    MachineConfig,
    MachineTarget,
    OpTarget,
    ToolProfile,
} from "./schema.js";
import { TOOL_PROFILES } from "./tools.js";

/**
 * Resolved axes: the 4 AxisConfig (x, y, z, a) + fCpu as a flat slice.
 * Z and A resolve to the default head. Used by wire/choreograph/discretize
 * which need the 4 axes but don't want to re-resolve the head on every call.
 */
export interface ResolvedAxes {
    readonly x: AxisConfig;
    readonly y: AxisConfig;
    readonly z: AxisConfig;
    readonly a: AxisConfig;
    readonly fCpu: number;
}

/** Resolve the 4 axes from a MachineConfig (Z/A from the default head). */
export function resolvedAxes(machine: MachineConfig): ResolvedAxes {
    const head = machine.heads[machine.defaultHead]!;
    return { x: machine.x, y: machine.y, z: head.z, a: head.a, fCpu: machine.fCpu };
}

/**
 * The four operation targets, with the tool→machine override chain already
 * applied. THE one place that chain is spelled out — stages read these fields
 * directly and must never write their own `??` fallback (that is how the
 * default cut feed ended up duplicated in four modules).
 *
 * `machine.*` is always populated (filled at load from DEFAULTS), so every
 * field here is defined except where the model itself says "unset":
 *   - accel is optional throughout — unset means "derive from axis ceilings",
 *     which is not the same as any particular number.
 *   - slew feed/accel unset means "use the A axis ceiling".
 *
 * Targets are still subject to per-axis clamping downstream; this resolves
 * WHAT was asked for, not what is physically permitted.
 */
export interface ResolvedTargets {
    /** Cut (pen-down) XY. Tool overrides machine. */
    readonly path: MachineTarget;
    /** Z engage (touch-down / retract). Tool overrides machine. */
    readonly z: MachineTarget;
    /** Pen-up XY reposition. Machine-owned — no tool override. */
    readonly rapid: MachineTarget;
    /** Standalone-A slew. Machine-owned — no tool override. */
    readonly slew: OpTarget;
}

/** Resolve all operation targets for a tool on a machine. See ResolvedTargets. */
export function resolveTargets(
    machine: MachineConfig,
    profile: ToolProfile,
): ResolvedTargets {
    return {
        path: {
            feed: profile.path?.feed ?? machine.path.feed,
            accel: profile.path?.accel ?? machine.path.accel,
        },
        z: {
            feed: profile.z?.feed ?? machine.z.feed,
            accel: profile.z?.accel ?? machine.z.accel,
        },
        rapid: machine.rapid,
        slew: machine.slew,
    };
}

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
