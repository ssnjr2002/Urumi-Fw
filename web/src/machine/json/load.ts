/**
 * load.ts — a config file's JSON → ControllerConfig (docs/system/config.md).
 *
 * A field is required exactly when it has no entry in DEFAULTS; anything else
 * absent takes its default. Tools patch the presets in tools.ts. Unknown keys
 * are ignored, except retired names that would change meaning silently, which
 * are refused by name.
 *
 *   parseConfig(json)      shape + required fields  → ControllerConfig | errors
 *   validateConfig(cfg)    semantics (ranges, ids)  → errors + warnings
 *   loadConfig(json)       parse ⨟ validate — the entry point callers want
 *
 * This pass judges shape only: a feed of 10^9 parses, and validate.ts refuses it.
 */

import {
    busNode,
    axisConfig,
    toolHead,
    machineConfig,
    toolProfile,
    qualityConfig,
    NodeType,
    type BusNode,
    type AxisConfig,
    type HomingConfig,
    type ProbeConfig,
    type OpTarget,
    type ToolHead,
    type ToolProfile,
    type ToolType,
    type QualityConfig,
    type ControllerConfig,
    type BedPoint,
    type ProbeSwitch,
    type LaserPointer,
    type StoredPositions,
    type WorkArea,
    type JobBuffer,
    type JobFeed,
} from "../schema.js";
import { TOOL_PROFILES } from "../tools.js";
import { validateConfig } from "./validate.js";
import { DEFAULTS } from "../defaults.js";

// ── JSON schema types (what the JSON looks like) ─────────────────────────────

interface JsonNode {
    readonly id: number;
    readonly type?: number;
    readonly present?: boolean;
}

interface JsonOpTarget {
    readonly feed?: number;
    readonly accel?: number;
}

interface JsonAxis {
    readonly node: JsonNode;
    readonly stepsPerUnit: number;
    readonly maxFeed?: number;
    readonly maxAccel?: number;
    readonly maxTravel?: number;
    readonly invertDir?: boolean;
    readonly softLimits?: boolean;
    readonly jogFeed?: number;
    readonly jogFeedUnhomed?: number;
    readonly rotary?: boolean;
    readonly homing?: JsonHoming;
    readonly probe?: JsonProbe;
    readonly unlockRange?: number;
}

interface JsonProbe {
    readonly node?: JsonNode;
    readonly switchXMm?: number;
    readonly switchYMm?: number;
    readonly replyDeadlineUs?: number;
    readonly probeTravel?: number;
    readonly pullInFeed?: number;
    readonly seekFeed?: number;
    readonly latchFeed?: number;
    readonly rampSteps?: number;
    readonly backoffMm?: number;
    readonly parkMm?: number;
    readonly seekOvertravelMm?: number;
    readonly tripMm?: number;
}

interface JsonHoming {
    readonly kind?: string;
    // linear
    readonly seekFeed?: number;
    readonly latchFeed?: number;
    // rotary
    readonly budgetRevs?: number;
    readonly sweepFeed?: number;
    readonly toleranceDeg?: number;
    // both
    readonly rampSteps?: number;
    // the controller's fields, carried through unchecked
    readonly cycle?: number;
    readonly startFeed?: number;
    readonly seekPositive?: boolean;
    readonly seekScaler?: number;
    readonly backoffDist?: number;
    readonly pullOffDist?: number;
    readonly parkPos?: number;
    readonly indexPos?: number;
}

interface JsonHead {
    /** Tool preset names this socket's fixture takes, preference-ordered. */
    readonly accepts: readonly string[];
    /** Removed — see the heads parse step. Typed so the check can see it. */
    readonly tool?: string;
    readonly xOffset?: number;
    readonly yOffset?: number;
    readonly probeSwitch?: JsonPoint & { readonly z?: number };
    /** Moved to `a`. Typed so the check can see it. */
    readonly unlockRange?: unknown;
    readonly z: JsonAxis;
    readonly a: JsonAxis;
}

interface JsonPoint {
    readonly x: number;
    readonly y: number;
}

interface JsonLaser {
    readonly node: number;
}

interface JsonMachine {
    readonly fCpu?: number;
    readonly feed?: {
        readonly cutFallback?: number;
        readonly travelDefault?: number;
        readonly plungeFallback?: number;
        readonly liftFeed?: number;
    };
    /** Removed — see buildFeed. Typed so the check can see them. */
    readonly path?: unknown;
    readonly rapid?: unknown;
    readonly z?: unknown;
    readonly slew?: JsonOpTarget;
    readonly zClearance?: number;
    readonly maxOvercut?: number;
    /** Renamed to zClearance. Typed so the check can see it. */
    readonly clearanceMm?: unknown;
    readonly x: JsonAxis;
    readonly y: JsonAxis;
    readonly laser?: JsonLaser;
    /** Removed: the work origin starts at workArea's min corner. Typed so the check can see it. */
    readonly work?: unknown;
    readonly positions?: {
        readonly park?: JsonPoint;
        readonly load?: JsonPoint;
        readonly mount?: JsonPoint;
    };
    readonly workArea?: { readonly xMin: number; readonly xMax: number; readonly yMin: number; readonly yMax: number };
    readonly jogUnhomed?: boolean;
    readonly meshOn: boolean;
    readonly buffer?: { readonly queueTime?: number; readonly idleTime?: number };
    readonly heads: readonly JsonHead[];
    readonly defaultHead?: number;
    readonly peripherals?: readonly JsonPeripheral[];
}

interface JsonPeripheral {
    readonly id: number;
    /** Required — see the peripherals build step for why it cannot default. */
    readonly type: number;
    readonly present?: boolean;
}

interface JsonToolOverride {
    readonly id?: number;
    readonly tangential?: boolean;
    readonly offsetMm?: number;
    readonly unwind?: boolean;
    readonly periodCount?: number;
    readonly cornerAngleDeg?: number;
    readonly minRadiusMm?: number;
    readonly aMaxDeg?: number;
    readonly aOffsetDeg?: number;
    readonly cutFeed?: number;
    readonly plungeFeed?: number;
    /** Removed — see the tools build step. Typed so the check can see them. */
    readonly path?: unknown;
    readonly z?: unknown;
    readonly plunge?: boolean;
    /** Removed — see the tools build step. Typed so the check can see it. */
    readonly liftHeight?: number;
    readonly toolOffset?: { xOffset: number; yOffset: number };
    readonly chamberOffsets?: readonly number[];
    /** Moved into `power` — see the tools build step. Typed so the check can see it. */
    readonly dutyLimits?: unknown;
    readonly power?: {
        node?: { id?: number; type?: number };
        settleS?: number;
        dutyLimits?: { maxOnS?: number; minOnS?: number; dwellS?: number };
    };
}

interface JsonQuality {
    readonly chordTol?: number;
    readonly dvMax?: number;
    readonly vMin?: number;
    readonly dtMax?: number;
    readonly dtMin?: number;
    readonly angleTol?: number;
    readonly gapTol?: number;
    readonly nKappa?: number;
    readonly junctionDeviation?: number;
    readonly dsMax?: number;
    readonly dthetaMax?: number;
    readonly maxRefine?: number;
}

interface ConfigJson {
    readonly machine: JsonMachine;
    /** Moved under `machine`. Typed so the check can see them. */
    readonly heads?: unknown;
    readonly defaultHead?: unknown;
    readonly peripherals?: unknown;
    readonly tools?: Readonly<Record<string, JsonToolOverride>>;
    readonly quality?: JsonQuality;
}

// ── result type ───────────────────────────────────────────────────────────────

export type ConfigResult =
    | { readonly ok: true; readonly config: ControllerConfig }
    | { readonly ok: false; readonly errors: readonly string[] };

// ── parser ────────────────────────────────────────────────────────────────────

/**
 * Parse a config file's JSON into a ControllerConfig. Lists every problem
 * found, not only the first.
 */
export function parseConfig(jsonText: string): ConfigResult {
    let raw: unknown;
    try {
        raw = JSON.parse(jsonText);
    } catch (e) {
        return err(`JSON parse error: ${(e as Error).message}`);
    }

    if (typeof raw !== "object" || raw === null || Array.isArray(raw)) {
        return err("config root must be an object");
    }
    const json = raw as ConfigJson;
    const errors: string[] = [];

    // ── machine ───────────────────────────────────────────────────────────
    if (typeof json.machine !== "object" || json.machine === null) {
        errors.push("machine: required (object)");
    } else {
        const m = json.machine;
        if (m.fCpu !== undefined && (typeof m.fCpu !== "number" || m.fCpu <= 0)) {
            errors.push("machine.fCpu: must be a positive number");
        }
        if (typeof m.x !== "object" || m.x === null) {
            errors.push("machine.x: required (axis object)");
        }
        if (typeof m.y !== "object" || m.y === null) {
            errors.push("machine.y: required (axis object)");
        }
        if (typeof m.meshOn !== "boolean") {
            errors.push("machine.meshOn: required (boolean)");
        }
    }

    // Rejected by name: under the lenient-unknown-keys rule a top-level `heads`
    // would read as a missing one, and a top-level `peripherals` as none.
    for (const key of ["heads", "defaultHead", "peripherals"] as const) {
        if (json[key] !== undefined) {
            errors.push(`${key}: moved — use machine.${key}`);
        }
    }

    // ── heads ─────────────────────────────────────────────────────────────
    const jsonHeads = (json.machine as JsonMachine | null | undefined)?.heads;
    if (!Array.isArray(jsonHeads) || jsonHeads.length === 0) {
        errors.push("machine.heads: required (non-empty array)");
    } else {
        for (let i = 0; i < jsonHeads.length; i++) {
            const h = jsonHeads[i]!;
            if (typeof h !== "object" || h === null) {
                errors.push(`machine.heads[${i}]: must be an object`);
                continue;
            }
            // `tool` named one seed tool and could not say what else fitted; refused by
            // name, since ignoring it would leave a head that accepts nothing.
            if (h.tool !== undefined) {
                errors.push(
                    `machine.heads[${i}].tool: removed — replace with 'accepts', the list of tool ` +
                        `presets this head's fixture takes (e.g. "accepts": ["${h.tool}"])`,
                );
            }
            if (!Array.isArray(h.accepts)) {
                errors.push(`machine.heads[${i}].accepts: required (array of tool preset names)`);
            } else {
                for (let k = 0; k < h.accepts.length; k++) {
                    const name = h.accepts[k];
                    if (typeof name !== "string") {
                        errors.push(`machine.heads[${i}].accepts[${k}]: must be a string`);
                    } else if (!(name in TOOL_PROFILES)) {
                        errors.push(
                            `machine.heads[${i}].accepts[${k}]: '${name}' is not a known tool preset`,
                        );
                    }
                }
            }
            if (typeof h.z !== "object" || h.z === null) {
                errors.push(`machine.heads[${i}].z: required (axis object)`);
            }
            if (typeof h.a !== "object" || h.a === null) {
                errors.push(`machine.heads[${i}].a: required (axis object)`);
            }
        }
    }

    // If structural errors, stop here — can't safely build objects.
    if (errors.length > 0) {
        return { ok: false, errors };
    }

    // ── build axes ────────────────────────────────────────────────────────
    const machine = json.machine!;
    const x = buildAxis(machine.x, errors, "machine.x");
    const y = buildAxis(machine.y, errors, "machine.y");

    const heads: ToolHead[] = [];
    for (let i = 0; i < machine.heads.length; i++) {
        const jh = machine.heads[i]!;
        const z = buildAxis(jh.z, errors, `machine.heads[${i}].z`);
        const a = buildAxis(jh.a, errors, `machine.heads[${i}].a`);
        const probeSwitch = buildSwitch(jh.probeSwitch, errors, `machine.heads[${i}].probeSwitch`);
        if (jh.unlockRange !== undefined) {
            errors.push(`machine.heads[${i}].unlockRange: moved — use machine.heads[${i}].a.unlockRange`);
        }
        heads.push(toolHead(z, a, {
            accepts: resolveAccepts(jh.accepts, errors, `machine.heads[${i}].accepts`),
            xOffset: jh.xOffset ?? 0,
            yOffset: jh.yOffset ?? 0,
            ...(probeSwitch ? { probeSwitch } : {}),
        }));
    }

    if (errors.length > 0) {
        return { ok: false, errors };
    }

    // ── build machine ─────────────────────────────────────────────────────
    // A peripheral's `type` is required: canRunTool matches tools against it, so
    // a default would make an unrunnable tool look runnable. `present` defaults
    // to true.
    const peripherals = (machine.peripherals ?? []).map((p, i) => {
        if (typeof p.id !== "number") {
            errors.push(`machine.peripherals[${i}].id: required (number)`);
        }
        if (typeof p.type !== "number") {
            errors.push(`machine.peripherals[${i}].type: required (number, a NODE_TYPE_* value)`);
        }
        if (typeof p.id !== "number" || typeof p.type !== "number") {
            return busNode(0, { present: false });
        }
        return busNode(p.id, {
            type: p.type as NodeType,
            present: p.present ?? true,
        });
    });

    if (errors.length > 0) {
        return { ok: false, errors };
    }

    const builtMachine = machineConfig(x, y, heads, {
        fCpu: machine.fCpu ?? DEFAULTS.machine.fCpu,
        // Fill-at-load: machine targets are always populated from DEFAULTS, so
        // consumers never write a `??` fallback. Tool overrides are NOT filled
        // (see patchToolProfile) — absent there means "inherit".
        feed: buildFeed(machine, errors),
        slew: opTarget(machine.slew, DEFAULTS.machine.slew),
        zClearance: buildZClearance(machine, errors),
        maxOvercut: buildMaxOvercut(machine, errors),
        peripherals,
        defaultHead: machine.defaultHead ?? 0,
        laser: buildLaser(machine.laser, errors),
        positions: buildPositions(machine.positions, errors),
        workArea: buildWorkArea(machine, errors),
        jogUnhomed: machine.jogUnhomed ?? false,
        meshOn: machine.meshOn === true,
        buffer: buildBuffer(machine.buffer, errors),
    });

    if (errors.length > 0) {
        return { ok: false, errors };
    }

    // ── build tool profiles registry ──────────────────────────────────────
    // Only the tools the config lists exist on the machine, each with its id.
    const toolProfiles: Record<string, ToolProfile> = {};
    if (json.tools) {
        for (const name of Object.keys(json.tools)) {
            if (!(name in TOOL_PROFILES)) {
                errors.push(`tools.${name}: not a known tool preset`);
                continue;
            }
            for (const [old, now] of [["path", "cutFeed"], ["z", "plungeFeed"]] as const) {
                if (json.tools[name]![old] !== undefined) {
                    errors.push(`tools.${name}.${old}: removed — use tools.${name}.${now}`);
                }
            }
            if (json.tools[name]!.dutyLimits !== undefined) {
                errors.push(
                    `tools.${name}.dutyLimits: moved — use tools.${name}.power.dutyLimits ` +
                    `(settleS goes to tools.${name}.power.settleS)`,
                );
            }
            if (json.tools[name]!.liftHeight !== undefined) {
                errors.push(
                    `tools.${name}.liftHeight: removed — set 'plunge'; the lift is ` +
                    "derived from the job's material thickness and machine.zClearance",
                );
                continue;
            }
            const id = json.tools[name]!.id;
            if (id === undefined) {
                errors.push(`tools.${name}.id: required`);
                continue;
            }
            const base = TOOL_PROFILES[name]!;
            toolProfiles[name] = patchToolProfile(name, base, json.tools);
        }
    }

    if (errors.length > 0) {
        return { ok: false, errors };
    }

    // ── build quality (code defaults + JSON overrides) ────────────────────
    const quality = qualityConfig(
        json.quality as Partial<QualityConfig> | undefined,
    );

    return { ok: true, config: { machine: builtMachine, quality, toolProfiles } };
}

// ── helpers ───────────────────────────────────────────────────────────────────

/** An optional {x, y}: undefined when absent, an error when malformed. */
function buildPoint(j: JsonPoint | undefined, errors: string[], at: string): BedPoint | undefined {
    if (j === undefined) return undefined;
    if (typeof j !== "object" || j === null || typeof j.x !== "number" || typeof j.y !== "number") {
        errors.push(`${at}: must be {x, y} in mm`);
        return undefined;
    }
    return { x: j.x, y: j.y };
}

function buildSwitch(
    j: (JsonPoint & { readonly z?: number }) | undefined,
    errors: string[],
    at: string,
): ProbeSwitch | undefined {
    const p = buildPoint(j, errors, at);
    if (p === undefined || j?.z === undefined) return p;
    if (typeof j.z !== "number") {
        errors.push(`${at}.z: must be a height in mm`);
        return undefined;
    }
    return { ...p, z: j.z };
}

function buildLaser(j: JsonLaser | null | undefined, errors: string[]): LaserPointer | undefined {
    if (j === null || j === undefined) return undefined;
    if (typeof j !== "object" || typeof j.node !== "number") {
        errors.push("machine.laser: must be {node} (bus id of the node it is wired to)");
        return undefined;
    }
    return { node: j.node };
}

// `work` was the work origin's default; rejected by name so an old config's
// origin does not move unnoticed.
function buildWorkArea(machine: JsonMachine, errors: string[]): WorkArea | undefined {
    if (machine.work !== undefined) errors.push("machine.work: removed — the work origin starts at machine.workArea's min corner");
    const j = machine.workArea;
    if (j === undefined) return undefined;
    if (typeof j !== "object" || j === null ||
        typeof j.xMin !== "number" || typeof j.xMax !== "number" ||
        typeof j.yMin !== "number" || typeof j.yMax !== "number") {
        errors.push("machine.workArea: must be {xMin, xMax, yMin, yMax} in mm");
        return undefined;
    }
    return { xMin: j.xMin, xMax: j.xMax, yMin: j.yMin, yMax: j.yMax };
}

function buildPositions(j: JsonMachine["positions"], errors: string[]): StoredPositions {
    const park = buildPoint(j?.park, errors, "machine.positions.park");
    const load = buildPoint(j?.load, errors, "machine.positions.load");
    const mount = buildPoint(j?.mount, errors, "machine.positions.mount");
    return {
        ...(park ? { park } : {}),
        ...(load ? { load } : {}),
        ...(mount ? { mount } : {}),
    };
}

/**
 * Build an OpTarget from JSON, keeping only numeric feed/accel and applying a
 * default for absent fields. Non-number JSON values are treated as absent.
 */
function opTarget(j: JsonOpTarget | undefined, fallback: JsonOpTarget): OpTarget {
    const out: { feed?: number; accel?: number } = {};
    const feed = typeof j?.feed === "number" ? j.feed : fallback.feed;
    const accel = typeof j?.accel === "number" ? j.accel : fallback.accel;
    if (feed !== undefined) out.feed = feed;
    if (accel !== undefined) out.accel = accel;
    return out;
}

// The machine's `path`, `rapid` and `z` became `feed`; rejected by name so an
// old config does not run on the defaults unnoticed.
function buildFeed(machine: JsonMachine, errors: string[]): JobFeed {
    if (machine.path !== undefined) errors.push("machine.path: removed — use machine.feed.cutFallback");
    if (machine.rapid !== undefined) errors.push("machine.rapid: removed — use machine.feed.travelDefault");
    if (machine.z !== undefined) errors.push("machine.z: removed — use machine.feed.plungeFallback");
    const j = machine.feed;
    const d = DEFAULTS.machine.feed;
    const feed = (k: keyof JobFeed): number => {
        const v = j?.[k];
        if (v === undefined) return d[k];
        if (typeof v !== "number") errors.push(`machine.feed.${k}: must be a number`);
        return v as number;
    };
    return {
        cutFallback: feed("cutFallback"),
        travelDefault: feed("travelDefault"),
        plungeFallback: feed("plungeFallback"),
        liftFeed: feed("liftFeed"),
    };
}

function buildZClearance(machine: JsonMachine, errors: string[]): number {
    if (machine.clearanceMm !== undefined) errors.push("machine.clearanceMm: renamed — use machine.zClearance");
    const v = machine.zClearance;
    if (v === undefined) return DEFAULTS.machine.zClearance;
    if (typeof v !== "number") errors.push("machine.zClearance: must be a number");
    return v;
}

function buildMaxOvercut(machine: JsonMachine, errors: string[]): number {
    const v = machine.maxOvercut;
    if (v === undefined) return DEFAULTS.machine.maxOvercut;
    if (typeof v !== "number") errors.push("machine.maxOvercut: must be a number");
    return v;
}

function buildBuffer(j: JsonMachine["buffer"], errors: string[]): JobBuffer {
    const d = DEFAULTS.machine.buffer;
    const time = (k: keyof JobBuffer): number => {
        const v = j?.[k];
        if (v === undefined) return d[k];
        if (typeof v !== "number") errors.push(`machine.buffer.${k}: must be a number`);
        return v as number;
    };
    return { queueTime: time("queueTime"), idleTime: time("idleTime") };
}

function buildAxis(ja: JsonAxis, errors: string[], path: string): AxisConfig {
    if (typeof ja.node !== "object" || ja.node === null || typeof ja.node.id !== "number") {
        errors.push(`${path}.node.id: required (number)`);
    }
    if (typeof ja.stepsPerUnit !== "number" || ja.stepsPerUnit <= 0) {
        errors.push(`${path}.stepsPerUnit: required (positive number)`);
    }
    if (errors.length > 0 && typeof ja.node !== "object") {
        // can't build — return a placeholder that won't be used
        return axisConfig(busNode(0, { present: false }), 1);
    }
    const node: BusNode = busNode(ja.node.id, {
        type: (ja.node.type ?? NodeType.STEPPER) as NodeType,
        present: ja.node.present ?? true,
    });
    const homing = buildHoming(ja.homing, errors, path);
    const probe = buildProbe(ja.probe, errors, path);
    return axisConfig(node, ja.stepsPerUnit, {
        maxFeed: ja.maxFeed ?? 0,
        maxAccel: ja.maxAccel ?? 0,
        maxTravel: ja.maxTravel ?? 0,
        // unlockRange is read on every axis so validate can refuse it off A.
        ...present(ja, ["invertDir", "softLimits", "unlockRange"]),
        jogFeed: ja.jogFeed ?? ja.maxFeed ?? 0,
        jogFeedUnhomed: ja.jogFeedUnhomed ?? ja.jogFeed ?? ja.maxFeed ?? 0,
        rotary: ja.rotary ?? false,
        // Spread, not `homing`, so an axis without a switch has no key at all
        // rather than an explicit `undefined`. `"homing" in axis` then means
        // what it says, and the optional field stays honest under exactOptional.
        ...(homing !== undefined ? { homing } : {}),
        ...(probe !== undefined ? { probe } : {}),
    });
}

/**
 * A probe block is all-or-nothing, as a homing block is: a missing field is an
 * error. `node`, the vacuum's id, is required too: a default would probe the
 * wrong node after the bus is renumbered.
 */
function buildProbe(
    jp: JsonProbe | undefined,
    errors: string[],
    path: string,
): ProbeConfig | undefined {
    if (jp === undefined) return undefined;
    const p = `${path}.probe`;
    if (typeof jp !== "object" || jp === null) {
        errors.push(`${p}: must be an object`);
        return undefined;
    }
    if (typeof jp.node !== "object" || jp.node === null ||
        typeof jp.node.id !== "number") {
        errors.push(`${p}.node: required — the bus id of the node the SWITCH is on`);
        return undefined;
    }
    const num = (k: keyof Omit<JsonProbe, "node">): number => {
        const v = jp[k];
        if (typeof v !== "number" || !Number.isFinite(v)) {
            errors.push(`${p}.${k}: required (number)`);
            return 0;
        }
        return v;
    };
    return {
        node: busNode(jp.node.id, {
            // The switch is on the vacuum; probe_map checks the type in the ENGAGE ack.
            type: (jp.node.type ?? NodeType.VACUUM) as NodeType,
            present: jp.node.present ?? true,
        }),
        switchXMm: num("switchXMm"),
        switchYMm: num("switchYMm"),
        replyDeadlineUs: num("replyDeadlineUs"),
        probeTravel: num("probeTravel"),
        pullInFeed: num("pullInFeed"),
        seekFeed: num("seekFeed"),
        latchFeed: num("latchFeed"),
        rampSteps: num("rampSteps"),
        backoffMm: num("backoffMm"),
        parkMm: num("parkMm"),
        seekOvertravelMm: num("seekOvertravelMm"),
        tripMm: num("tripMm"),
    };
}

/**
 * A homing block is all-or-nothing: absent means no terminator, present means
 * every field of its kind and none of the other. Each number is a measurement
 * of this machine, so a missing one is an error, never defaulted.
 */
function buildHoming(
    jh: JsonHoming | undefined,
    errors: string[],
    path: string,
): HomingConfig | undefined {
    if (jh === undefined) return undefined;
    const p = `${path}.homing`;
    if (typeof jh !== "object" || jh === null) {
        errors.push(`${p}: must be an object`);
        return undefined;
    }
    const kind = jh.kind ?? "linear";
    if (kind !== "linear" && kind !== "rotary") {
        errors.push(`${p}.kind: must be "linear" or "rotary"`);
        return undefined;
    }
    const num = (k: keyof JsonHoming): number => {
        const v = jh[k];
        if (typeof v !== "number" || !Number.isFinite(v)) {
            errors.push(`${p}.${k}: required (number)`);
            return 0;
        }
        return v;
    };
    if (kind === "rotary") {
        // Refused by name: a linear field in a rotary block means its writer thought
        // the axis has ends.
        for (const k of ["seekFeed", "latchFeed"] as const) {
            if (jh[k] !== undefined) {
                errors.push(`${p}.${k}: not a rotary field — a rotary axis has no ends`);
            }
        }
        return {
            kind: "rotary",
            budgetRevs: num("budgetRevs"),
            sweepFeed: num("sweepFeed"),
            rampSteps: num("rampSteps"),
            toleranceDeg: num("toleranceDeg"),
            ...present(jh, ["cycle", "startFeed", "indexPos"]),
        };
    }
    for (const k of ["budgetRevs", "sweepFeed", "toleranceDeg"] as const) {
        if (jh[k] !== undefined) {
            errors.push(`${p}.${k}: not a linear field — did you mean kind "rotary"?`);
        }
    }
    return {
        kind: "linear",
        seekFeed: num("seekFeed"),
        latchFeed: num("latchFeed"),
        rampSteps: num("rampSteps"),
        ...present(jh, ["cycle", "seekPositive", "seekScaler", "startFeed",
                        "backoffDist", "pullOffDist", "parkPos"]),
    };
}

/**
 * The fields of `o` named in `keys` that are present. The controller's fields
 * pass through this way, checked by its decoder rather than the host.
 */
function present<T extends object, K extends keyof T>(
    o: T,
    keys: readonly K[],
): { [P in K]?: Exclude<T[P], undefined> } {
    const out: { [P in K]?: Exclude<T[P], undefined> } = {};
    for (const k of keys) {
        if (o[k] !== undefined) out[k] = o[k] as Exclude<T[K], undefined>;
    }
    return out;
}

/**
 * Tool preset names → ToolTypes, preserving order.
 *
 * Re-checks that a preset name and a ToolType identify each other. It cannot
 * fire today; it trips if user-defined profiles are ever allowed, since two
 * profiles sharing a type would read as one tool (or as revolver chambers).
 */
function resolveAccepts(
    names: readonly string[],
    errors: string[],
    path: string,
): ToolType[] {
    const out: ToolType[] = [];
    const seen = new Map<ToolType, string>();
    names.forEach((name, k) => {
        const type = TOOL_PROFILES[name]!.toolType;
        const owner = seen.get(type);
        if (owner !== undefined) {
            errors.push(
                owner === name
                    ? `${path}[${k}]: '${name}' listed twice`
                    : `${path}[${k}]: '${name}' and '${owner}' share tool type ${type} — ` +
                      "a tool type must identify exactly one profile",
            );
            return;
        }
        seen.set(type, name);
        out.push(type);
    });
    return out;
}

function patchToolProfile(
    name: string,
    base: ToolProfile,
    tools: Readonly<Record<string, JsonToolOverride>> | undefined,
): ToolProfile {
    if (!tools || !(name in tools)) return base;
    const o = tools[name]!;

    // Spread, so a field added to ToolProfile later is kept (load.test.ts checks).
    const merged: Record<string, unknown> = { ...base };
    delete merged.name; // supplied by toolProfile()

    for (const key of ["tangential", "offsetMm", "unwind", "periodCount", "cornerAngleDeg",
                       "minRadiusMm", "aMaxDeg", "aOffsetDeg", "plunge", "toolOffset", "chamberOffsets", "id",
                       "cutFeed", "plungeFeed"] as const) {
        if (o[key] !== undefined) merged[key] = o[key];
    }

    // Missing numbers become 0 rather than an invented default: these are
    // measurements of a specific tool, and validate rejects a zero node, budget
    // or dwell with a message naming the field.
    const p = o.power;
    if (p !== undefined) {
        const d = p.dutyLimits;
        merged.power = {
            node: { id: p.node?.id ?? 0, type: p.node?.type ?? 0 },
            settleS: p.settleS ?? 0,
            ...(d !== undefined && {
                dutyLimits: { maxOnS: d.maxOnS ?? 0, minOnS: d.minOnS ?? 0, dwellS: d.dwellS ?? 0 },
            }),
        };
    }

    return toolProfile(name, merged as Partial<Omit<ToolProfile, "name">>);
}

function err(msg: string): ConfigResult {
    return { ok: false, errors: [msg] };
}

// ── entry point ───────────────────────────────────────────────────────────────

export type LoadResult =
    | {
          readonly ok: true;
          readonly config: ControllerConfig;
          /** Non-fatal advisories (e.g. a target above its axis ceiling). */
          readonly warnings: readonly string[];
      }
    | { readonly ok: false; readonly errors: readonly string[] };

/**
 * Parse and validate a config file's JSON: the entry point outside tests.
 * `ok: true` means well-formed and sane; warnings are returned, never enforced.
 */
export function loadConfig(jsonText: string): LoadResult {
    const parsed = parseConfig(jsonText);
    if (!parsed.ok) return parsed;

    const { errors, warnings } = validateConfig(parsed.config);
    if (errors.length > 0) return { ok: false, errors };
    return { ok: true, config: parsed.config, warnings };
}
