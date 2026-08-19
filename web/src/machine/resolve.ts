/**
 * resolve.ts — resolution POLICY over the config data.
 *
 * Everything that answers "given this config, what actually applies?":
 *   - axesForHead / resolvedAxesDefault — the 4 live axes, Z/A per head
 *   - headsAccepting — which sockets a tool's fixture fits
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
    ToolType,
} from "./schema.js";
import { TOOL_PROFILES } from "./tools.js";

/**
 * Resolved axes: the 4 AxisConfig (x, y, z, a) + fCpu as a flat slice.
 * X/Y are the shared gantry; Z and A belong to ONE head, named by whoever
 * resolved them. Used by wire/choreograph/discretize, which need the 4 axes
 * but must not re-resolve the head on every call.
 */
export interface ResolvedAxes {
    readonly x: AxisConfig;
    readonly y: AxisConfig;
    readonly z: AxisConfig;
    readonly a: AxisConfig;
    readonly fCpu: number;
}

/**
 * The 4 axes with Z/A taken from `head`. The shared primitive: anything that
 * converts mm to steps on a Z or an A must go through here, naming the head it
 * means, because stepsPerUnit/invert/maxFeed/maxAccel are all per-head and a
 * trajectory is shaped by all four.
 *
 * Throws on a head the machine does not have rather than clamping — a caller
 * asking for head 2 of a two-head machine has a bug, and silently handing it
 * head 1's calibration is exactly the wrong-cut-no-exception failure this
 * function exists to prevent.
 */
export function axesForHead(machine: MachineConfig, head: number): ResolvedAxes {
    const h = machine.heads[head];
    if (!h) throw new RangeError(`no head ${head} (machine has ${machine.heads.length})`);
    return { x: machine.x, y: machine.y, z: h.z, a: h.a, fCpu: machine.fCpu };
}

/**
 * The 4 axes resolved against `defaultHead`.
 *
 * Named for what it does, because what it does is usually not what a caller
 * wants: on a dual-head machine it answers with one head's calibration no
 * matter which head the work is destined for. Legitimate uses are the ones
 * genuinely indifferent to the head (fCpu, X/Y) or genuinely about the initial
 * binding. Anything per-block wants `axesForHead`.
 */
export function resolvedAxesDefault(machine: MachineConfig): ResolvedAxes {
    return axesForHead(machine, machine.defaultHead);
}

/**
 * Head indices whose fixture accepts `tool`, in machine order.
 *
 * Empty means no head can hold it — a config-level impossibility worth naming
 * at the boundary, not a scheduling failure to work around.
 */
export function headsAccepting(machine: MachineConfig, tool: ToolType): number[] {
    const out: number[] = [];
    machine.heads.forEach((h, i) => {
        if (h.accepts.includes(tool)) out.push(i);
    });
    return out;
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
