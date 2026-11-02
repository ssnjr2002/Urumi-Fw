/**
 * runtime.ts — the config fields the Pico can edit while it runs.
 *
 * A row is a template over its scope: an axis row covers every axis, a tool row
 * every tool. Its path is relative to the scope's type, which types it; a
 * concrete field's blob path adds the scope's prefix (`machine.heads.0.z.`,
 * `toolProfiles.knife.`). `pnpm gen:runtime` writes the Pico's copy, see
 * runtimeHeader.ts and docs/plans/runtime-config.md.
 */

import { fitsF32 } from "./f32.js";
import type {
    AxisConfig,
    ControllerConfig,
    MachineConfig,
    ToolHead,
    ToolProfile,
} from "./schema.js";

/** Every dotted path into `T`, through optional fields and each arm of a union. */
export type PathOf<T> = T extends readonly unknown[]
    ? never
    : T extends object
      ? {
            [K in keyof T & string]-?: K | `${K}.${PathOf<NonNullable<T[K]>>}`;
        }[keyof T & string]
      : never;

export const RUNTIME_EFFECTS = [
    "voidDatum",
    "applyMesh",
    "republishBuffer",
    "refreshFrames",
    "refreshTool",
] as const;

export type RuntimeEffect = (typeof RUNTIME_EFFECTS)[number];
export type RuntimeScope = "machine" | "axis" | "head" | "tool";
export type RuntimeType = "f32" | "bool" | "u16" | "point";

export interface RuntimeField {
    readonly scope: RuntimeScope;
    /** Relative to the scope. */
    readonly path: string;
    /** The C++ member, relative to the scope's struct; the path, unless it differs. */
    readonly member: string;
    /** The C++ flag saying an optional field is set. Absent on a required field. */
    readonly has?: string;
    readonly type: RuntimeType;
    /** Inclusive, unless minExclusive. */
    readonly min?: number;
    readonly minExclusive?: boolean;
    readonly max?: number;
    /** A homing row: the homing kinds that have this field. */
    readonly homing?: "linear" | "rotary" | "both";
    readonly effects: readonly RuntimeEffect[];
}

type RowOptions = Partial<Omit<RuntimeField, "scope" | "path" | "member" | "effects">> & {
    readonly effects?: readonly RuntimeEffect[];
};

function row(scope: RuntimeScope, path: string, o: RowOptions): RuntimeField {
    return { scope, path, member: path, type: "f32", effects: [], ...o };
}

const machine = (path: PathOf<MachineConfig>, o: RowOptions = {}) => row("machine", path, o);
const axis = (path: PathOf<AxisConfig>, o: RowOptions = {}) => row("axis", path, o);
const head = (path: PathOf<ToolHead>, o: RowOptions = {}) => row("head", path, o);
const tool = (path: PathOf<ToolProfile>, o: RowOptions = {}) => row("tool", path, o);

const NON_NEGATIVE = { min: 0 } as const;
const POSITIVE = { min: 0, minExclusive: true } as const;
const RAMP = { type: "u16", min: 0, max: 65535 } as const;

export const RUNTIME_FIELDS: readonly RuntimeField[] = [
    machine("positions.park", { type: "point", has: "positions.hasPark" }),
    machine("positions.load", { type: "point", has: "positions.hasLoad" }),
    machine("positions.mount", { type: "point", has: "positions.hasMount" }),
    machine("feed.cutFallback", { min: 1 }),
    machine("feed.travelDefault", { min: 1 }),
    machine("feed.plungeFallback", { min: 1 }),
    machine("feed.liftFeed", { min: 1 }),
    machine("buffer.queueTime", { ...POSITIVE, effects: ["republishBuffer"] }),
    machine("buffer.idleTime", { ...POSITIVE, effects: ["republishBuffer"] }),
    machine("zClearance", NON_NEGATIVE),
    machine("maxOvercut", NON_NEGATIVE),
    machine("meshOn", { type: "bool", effects: ["applyMesh"] }),

    axis("stepsPerUnit", { ...POSITIVE, effects: ["voidDatum"] }),
    axis("invertDir", { type: "bool", effects: ["voidDatum"] }),
    axis("maxTravel", { ...NON_NEGATIVE, effects: ["voidDatum"] }),
    axis("maxFeed", NON_NEGATIVE),
    axis("maxAccel", NON_NEGATIVE),
    axis("jogFeed", NON_NEGATIVE),
    axis("jogFeedUnhomed", NON_NEGATIVE),

    axis("homing.startFeed", { ...POSITIVE, homing: "both" }),
    axis("homing.rampSteps", { ...RAMP, homing: "both" }),
    axis("homing.seekFeed", { ...POSITIVE, homing: "linear" }),
    axis("homing.latchFeed", { ...POSITIVE, homing: "linear" }),
    axis("homing.seekScaler", { min: 1, homing: "linear" }),
    axis("homing.backoffDist", { ...POSITIVE, homing: "linear" }),
    axis("homing.pullOffDist", { ...POSITIVE, homing: "linear", effects: ["voidDatum"] }),
    axis("homing.parkPos", { has: "homing.hasParkPos", homing: "linear", effects: ["voidDatum"] }),
    axis("homing.sweepFeed", { ...POSITIVE, homing: "rotary" }),
    // The measured floor: under it a healthy axis at the wrong phase reports notfound.
    axis("homing.budgetRevs", { min: 3.2, homing: "rotary" }),
    axis("homing.toleranceDeg", { ...POSITIVE, homing: "rotary" }),
    axis("homing.indexPos", { homing: "rotary", effects: ["voidDatum"] }),

    head("xOffset", { effects: ["refreshFrames"] }),
    head("yOffset", { effects: ["refreshFrames"] }),
    head("probeSwitch", { type: "point", has: "hasProbeSwitch" }),
    head("probeSwitch.z", { ...NON_NEGATIVE, has: "probeSwitch.hasZ" }),
    head("unlockRange", { ...NON_NEGATIVE, has: "hasUnlockRange" }),

    tool("cutFeed", { ...NON_NEGATIVE, has: "hasCutFeed" }),
    tool("plungeFeed", { ...NON_NEGATIVE, has: "hasPlungeFeed" }),
    tool("toolOffset.xOffset", { effects: ["refreshFrames"] }),
    tool("toolOffset.yOffset", { effects: ["refreshFrames"] }),
    tool("offsetMm", { ...NON_NEGATIVE, has: "hasOffsetMm", effects: ["refreshTool"] }),
    tool("aOffsetDeg", { has: "hasAOffsetDeg", effects: ["refreshTool"] }),
    tool("aMaxDeg", { ...NON_NEGATIVE, has: "hasAMaxDeg", effects: ["refreshTool"] }),
    tool("cornerAngleDeg", { min: 0, max: 180, effects: ["refreshTool"] }),
];

/** A row applied to one axis, head or tool of a config. */
export interface RuntimeValue {
    readonly field: RuntimeField;
    /** The blob path. */
    readonly path: string;
    /** The path as the config file spells it, for messages. */
    readonly label: string;
    /** Undefined when an optional field is absent. */
    readonly value: unknown;
}

function read(root: object, path: string): unknown {
    let at: unknown = root;
    for (const key of path.split(".")) {
        at = (at as Record<string, unknown> | undefined)?.[key];
    }
    return at;
}

/** Every concrete field of `config`, without the homing rows its axes lack. */
export function runtimeValues(config: ControllerConfig): RuntimeValue[] {
    const m = config.machine;
    const axes: [string, string, AxisConfig][] = [
        ["machine.x", "machine.x", m.x],
        ["machine.y", "machine.y", m.y],
        ...m.heads.flatMap((h, i): [string, string, AxisConfig][] => [
            [`machine.heads.${i}.z`, `machine.heads[${i}].z`, h.z],
            [`machine.heads.${i}.a`, `machine.heads[${i}].a`, h.a],
        ]),
    ];
    return RUNTIME_FIELDS.flatMap((field): RuntimeValue[] => {
        const at = (prefix: string, label: string, root: object): RuntimeValue => ({
            field,
            path: `${prefix}.${field.path}`,
            label: `${label}.${field.path}`,
            value: read(root, field.path),
        });
        switch (field.scope) {
            case "machine":
                return [at("machine", "machine", m)];
            case "axis":
                return axes
                    .filter(([, , ax]) => homingHas(field, ax))
                    .map(([path, label, ax]) => at(path, label, ax));
            case "head":
                return m.heads.map((h, i) => at(`machine.heads.${i}`, `machine.heads[${i}]`, h));
            case "tool":
                return Object.entries(config.toolProfiles).map(([name, t]) =>
                    at(`toolProfiles.${name}`, `tools.${name}`, t),
                );
        }
    });
}

function homingHas(field: RuntimeField, ax: AxisConfig): boolean {
    if (field.homing === undefined) return true;
    if (ax.homing === undefined) return false;
    return field.homing === "both" || field.homing === ax.homing.kind;
}

/** What is wrong with `value` for this field, or null. */
export function runtimeFieldError(field: RuntimeField, value: unknown): string | null {
    switch (field.type) {
        case "bool":
            return typeof value === "boolean" ? null : "must be true or false";
        case "point": {
            const p = value as { x?: unknown; y?: unknown } | null;
            return numberError(field, p?.x) ?? numberError(field, p?.y);
        }
        case "u16":
            return Number.isInteger(value) ? numberError(field, value) : `must be a whole number (got ${String(value)})`;
        case "f32":
            return numberError(field, value);
    }
}

function numberError(field: RuntimeField, v: unknown): string | null {
    if (typeof v !== "number" || !Number.isFinite(v)) return `must be a finite number (got ${String(v)})`;
    const { min, max, minExclusive } = field;
    if (min !== undefined && (minExclusive ? v <= min : v < min)) {
        return `must be ${minExclusive ? ">" : ">="} ${min} (got ${v})`;
    }
    if (max !== undefined && v > max) return `must be <= ${max} (got ${v})`;
    return fitsF32(v) ? null : `${v} has more precision than the controller keeps`;
}
