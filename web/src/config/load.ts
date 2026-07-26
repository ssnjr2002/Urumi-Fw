/**
 * load.ts — JSON → PipelineConfig. STRUCTURE ONLY.
 *
 * The production config source: a config.json file carries all
 * machine-specific calibration (stepsPerUnit, fCpu, invert, node bindings,
 * head layout, laser pointer). The code provides universal defaults —
 * tool presets (PEN/KNIFE/CREASE/REVOLVER_PEN) and quality algorithm
 * tuning — which the JSON can override but does not redefine from scratch.
 *
 * Required vs optional:
 *   Required: machine.x, machine.y, heads[], each axis's node.id +
 *   stepsPerUnit, each head's tool. Missing → error. Note the rule: a field is
 *   required IFF it has no entry in DEFAULTS. Requiring a field that also has a
 *   default is a contradiction — the default could never be reached.
 *
 *   Optional: machine.fCpu (a property of the master board, not per-machine
 *   calibration — constant at 150 MHz across RP2350 boards, so defaulting it
 *   beats making every config.json restate it), machine targets
 *   (path/rapid/z/slew), axis ceilings
 *   (maxFeed/maxAccel), invert, maxTravel, laser, peripherals (the ARRAY is
 *   optional, but each entry needs id + type), tools.*,
 *   quality. Absent → documented code default. See feed_accel_value_model.md.
 *
 * Lenient migration: unknown keys (old maxRate/accel/feedMax/jogFeed/zFeed/
 * nodeId names) are silently ignored, not rejected.
 *
 * No silent fallback to hardcoded machine calibration (the old
 * defaultMachine() with 160/1200/51.667 is a TEST FIXTURE only, not
 * the production path). The production path is loadConfig(json).
 *
 * Parse, then validate — two passes, never interleaved:
 *
 *   parseConfig(json)      shape + required fields  → PipelineConfig | errors
 *   validateConfig(cfg)    semantics (ranges, ids)  → errors + warnings
 *   loadConfig(json)       parse ⨟ validate — the entry point callers want
 *
 * This module answers only "is this a well-formed config?" — it must not judge
 * whether the VALUES make sense. A feed of 10^9 parses fine here and is
 * validate.ts's problem. Keeping the split honest is what lets each pass be
 * read, tested, and extended on its own.
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
    type OpTarget,
    type MachineTarget,
    type ToolHead,
    type ToolProfile,
    type QualityConfig,
    type PipelineConfig,
} from "./schema.js";
import { TOOL_PROFILES } from "./tools.js";
import { validateConfig } from "./validate.js";
import { DEFAULTS } from "./defaults.js";

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
    readonly invert?: boolean;
    readonly rotary?: boolean;
}

interface JsonHead {
    readonly tool: string;
    readonly xOffset?: number;
    readonly yOffset?: number;
    readonly z: JsonAxis;
    readonly a: JsonAxis;
}

interface JsonLaser {
    readonly xOffset: number;
    readonly yOffset: number;
}

interface JsonMachine {
    readonly fCpu?: number;
    readonly path?: JsonOpTarget;
    readonly rapid?: JsonOpTarget;
    readonly z?: JsonOpTarget;
    readonly slew?: JsonOpTarget;
    readonly x: JsonAxis;
    readonly y: JsonAxis;
    readonly laser?: JsonLaser;
}

interface JsonPeripheral {
    readonly id: number;
    /** Required — see the peripherals build step for why it cannot default. */
    readonly type: number;
    readonly present?: boolean;
}

interface JsonToolOverride {
    readonly tangential?: boolean;
    readonly offsetMm?: number;
    readonly unwind?: boolean;
    readonly cornerAngleDeg?: number;
    readonly minRadiusMm?: number;
    readonly path?: JsonOpTarget;
    readonly z?: JsonOpTarget;
    readonly liftHeight?: number;
    readonly toolOffset?: { xOffset: number; yOffset: number };
    readonly slotOffsets?: readonly number[];
    readonly dutyLimits?: {
        maxOnS?: number; minOnS?: number; dwellS?: number; settleS?: number;
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
}

interface ConfigJson {
    readonly machine: JsonMachine;
    readonly heads: readonly JsonHead[];
    readonly defaultHead?: number;
    readonly peripherals?: readonly JsonPeripheral[];
    readonly tools?: Readonly<Record<string, JsonToolOverride>>;
    readonly quality?: JsonQuality;
}

// ── result type ───────────────────────────────────────────────────────────────

export type ConfigResult =
    | { readonly ok: true; readonly config: PipelineConfig }
    | { readonly ok: false; readonly errors: readonly string[] };

// ── parser ────────────────────────────────────────────────────────────────────

/**
 * Parse a config.json string into a PipelineConfig.
 *
 * Returns { ok: true, config } on success, or { ok: false, errors } with
 * a list of every problem found (not just the first). The caller decides
 * how to surface errors to the operator.
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
    }

    // ── heads ─────────────────────────────────────────────────────────────
    if (!Array.isArray(json.heads) || json.heads.length === 0) {
        errors.push("heads: required (non-empty array)");
    } else {
        for (let i = 0; i < json.heads.length; i++) {
            const h = json.heads[i]!;
            if (typeof h !== "object" || h === null) {
                errors.push(`heads[${i}]: must be an object`);
                continue;
            }
            if (typeof h.tool !== "string") {
                errors.push(`heads[${i}].tool: required (string)`);
            } else if (!(h.tool in TOOL_PROFILES)) {
                errors.push(`heads[${i}].tool: '${h.tool}' is not a known tool preset`);
            }
            if (typeof h.z !== "object" || h.z === null) {
                errors.push(`heads[${i}].z: required (axis object)`);
            }
            if (typeof h.a !== "object" || h.a === null) {
                errors.push(`heads[${i}].a: required (axis object)`);
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
    for (let i = 0; i < json.heads.length; i++) {
        const jh = json.heads[i]!;
        const z = buildAxis(jh.z, errors, `heads[${i}].z`);
        const a = buildAxis(jh.a, errors, `heads[${i}].a`);
        const toolName = jh.tool!;
        const baseProfile = TOOL_PROFILES[toolName]!;
        const patchedProfile = patchToolProfile(toolName, baseProfile, json.tools);
        heads.push(toolHead(z, a, {
            profile: patchedProfile,
            xOffset: jh.xOffset ?? 0,
            yOffset: jh.yOffset ?? 0,
        }));
    }

    if (errors.length > 0) {
        return { ok: false, errors };
    }

    // ── build machine ─────────────────────────────────────────────────────
    // A peripheral's `type` is REQUIRED and has no default. Defaulting it to
    // STEPPER was actively wrong: a peripheral is by definition the non-axis
    // case, so the one value it can never sensibly be is the one it defaulted
    // to. Type is also the whole reason the entry exists — canRunTool matches a
    // tool's requiredPeripheralTypes against it, so a wrong type silently makes
    // an unrunnable tool look runnable. `present` still defaults to true: a
    // declared peripheral is fitted unless stated otherwise.
    const peripherals = (json.peripherals ?? []).map((p, i) => {
        if (typeof p.id !== "number") {
            errors.push(`peripherals[${i}].id: required (number)`);
        }
        if (typeof p.type !== "number") {
            errors.push(`peripherals[${i}].type: required (number, a NODE_TYPE_* value)`);
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
        path: machineTarget(machine.path, DEFAULTS.machine.path),
        rapid: machineTarget(machine.rapid, DEFAULTS.machine.rapid),
        z: machineTarget(machine.z, DEFAULTS.machine.z),
        slew: opTarget(machine.slew, DEFAULTS.machine.slew),
        peripherals,
        defaultHead: json.defaultHead ?? 0,
        laser: machine.laser ?? undefined,
    });

    // ── build tool profiles registry ──────────────────────────────────────
    const toolProfiles: Record<string, ToolProfile> = { ...TOOL_PROFILES };
    if (json.tools) {
        for (const name of Object.keys(json.tools)) {
            if (!(name in TOOL_PROFILES)) {
                errors.push(`tools.${name}: not a known tool preset`);
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

/**
 * As opTarget, but for the machine tier where a feed is guaranteed. The
 * fallback's feed is required, so the result satisfies MachineTarget.
 */
function machineTarget(
    j: JsonOpTarget | undefined,
    fallback: { readonly feed: number; readonly accel?: number },
): MachineTarget {
    const feed = typeof j?.feed === "number" ? j.feed : fallback.feed;
    const accel = typeof j?.accel === "number" ? j.accel : fallback.accel;
    return accel !== undefined ? { feed, accel } : { feed };
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
    return axisConfig(node, ja.stepsPerUnit, {
        maxFeed: ja.maxFeed ?? 0,
        maxAccel: ja.maxAccel ?? 0,
        maxTravel: ja.maxTravel ?? 0,
        invert: ja.invert ?? false,
        rotary: ja.rotary ?? false,
    });
}

function patchToolProfile(
    name: string,
    base: ToolProfile,
    tools: Readonly<Record<string, JsonToolOverride>> | undefined,
): ToolProfile {
    if (!tools || !(name in tools)) return base;
    const o = tools[name]!;

    // Spread, don't hand-list. The previous version enumerated every field of
    // ToolProfile by name, which silently dropped any field added later —
    // an override of one key would reset the new key to its generic default,
    // with no error. Spreading `base` makes the merge field-count-agnostic;
    // load.test.ts's key-driven no-op test locks that property in.
    const merged: Record<string, unknown> = { ...base };
    delete merged.name; // supplied by toolProfile()

    for (const key of ["tangential", "offsetMm", "unwind", "cornerAngleDeg",
                       "minRadiusMm", "liftHeight", "toolOffset", "slotOffsets"] as const) {
        if (o[key] !== undefined) merged[key] = o[key];
    }
    // Engage targets merge FIELD-WISE over the preset: `{accel}` alone must not
    // wipe the preset's feed. Tool tier is never filled from DEFAULTS — an
    // absent target stays undefined, meaning "inherit" (see resolveTargets).
    if (o.path !== undefined) merged.path = opTarget(o.path, base.path ?? {});
    if (o.z !== undefined) merged.z = opTarget(o.z, base.z ?? {});

    // Duty limits merge field-wise over the preset, same rule as the targets.
    // Missing numbers become 0 rather than an invented default: these are
    // measurements of a specific tool, and validate rejects a zero budget or
    // dwell with a message naming the field. Guessing them here would produce
    // a config that runs and cuts wrong.
    if (o.dutyLimits !== undefined) {
        const b = base.dutyLimits;
        merged.dutyLimits = {
            maxOnS:  o.dutyLimits.maxOnS  ?? b?.maxOnS  ?? 0,
            minOnS:  o.dutyLimits.minOnS  ?? b?.minOnS  ?? 0,
            dwellS:  o.dutyLimits.dwellS  ?? b?.dwellS  ?? 0,
            settleS: o.dutyLimits.settleS ?? b?.settleS ?? 0,
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
          readonly config: PipelineConfig;
          /** Non-fatal advisories (e.g. a target above its axis ceiling). */
          readonly warnings: readonly string[];
      }
    | { readonly ok: false; readonly errors: readonly string[] };

/**
 * Parse AND validate a config.json. This is the entry point production callers
 * want: `ok: true` means the config is well-formed *and* semantically sane.
 *
 * parseConfig alone proves only the former, which is why it should not be
 * called directly outside tests — a config with a negative feed or duplicate
 * node ids parses perfectly and then misbehaves on the machine.
 *
 * Warnings never block: they are surfaced to the operator, not enforced.
 */
export function loadConfig(jsonText: string): LoadResult {
    const parsed = parseConfig(jsonText);
    if (!parsed.ok) return parsed;

    const { errors, warnings } = validateConfig(parsed.config);
    if (errors.length > 0) return { ok: false, errors };
    return { ok: true, config: parsed.config, warnings };
}
