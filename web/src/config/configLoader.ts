/**
 * configLoader.ts — JSON → PipelineConfig parser.
 *
 * The production config source: a config.json file carries all
 * machine-specific calibration (stepsPerUnit, fCpu, invert, node bindings,
 * head layout, laser pointer). The code provides universal defaults —
 * tool presets (PEN/KNIFE/CREASE/REVOLVER_PEN) and quality algorithm
 * tuning — which the JSON can override but does not redefine from scratch.
 *
 * Required vs optional:
 *   Required: machine.fCpu, machine.x, machine.y, heads[], each axis's
 *   node.id + stepsPerUnit, each head's tool. Missing → error.
 *
 *   Optional: machine targets (path/rapid/z/slew), axis ceilings
 *   (maxFeed/maxAccel), invert, maxTravel, laser, peripherals, tools.*,
 *   quality. Absent → documented code default. See feed_accel_value_model.md.
 *
 * Lenient migration: unknown keys (old maxRate/accel/feedMax/jogFeed/zFeed/
 * nodeId names) are silently ignored, not rejected.
 *
 * No silent fallback to hardcoded machine calibration (the old
 * defaultMachine() with 160/1200/51.667 is a TEST FIXTURE only, not
 * the production path). The production path is configLoader.parse(json).
 *
 * Validation (range checks, duplicate node IDs) is deferred — this module
 * checks required fields and structural shape only. A separate validate()
 * pass can be added later.
 */

import {
    busNode,
    axisConfig,
    toolHead,
    machineConfig,
    toolProfile,
    qualityConfig,
    pipelineConfig,
    NodeType,
    TOOL_PROFILES,
    type BusNode,
    type AxisConfig,
    type OpTarget,
    type ToolHead,
    type ToolProfile,
    type QualityConfig,
    type PipelineConfig,
} from "./config.js";

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
    readonly fCpu: number;
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
    readonly type?: number;
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
        if (typeof m.fCpu !== "number" || m.fCpu <= 0) {
            errors.push("machine.fCpu: required (positive number)");
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
    const peripherals = (json.peripherals ?? []).map((p, i) => {
        if (typeof p.id !== "number") {
            errors.push(`peripherals[${i}].id: required (number)`);
            return busNode(0, { present: false });
        }
        //  TODO: Deliberate if these defaults are good. Currently I am not convinced they are.
        return busNode(p.id, {
            type: (p.type ?? NodeType.STEPPER) as NodeType,
            present: p.present ?? true,
        });
    });

    if (errors.length > 0) {
        return { ok: false, errors };
    }

    const builtMachine = machineConfig(x, y, heads, {
        fCpu: machine.fCpu,
        path: opTarget(machine.path, { feed: 80 }),
        rapid: opTarget(machine.rapid, { feed: 80 }),
        z: opTarget(machine.z, { feed: 20 }),
        slew: opTarget(machine.slew, {}),
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

    return { ok: true, config: pipelineConfig({ machine: builtMachine, quality, toolProfiles }) };
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

    // Start from the base preset's values (mutable copy), apply JSON overrides.
    const merged: Record<string, unknown> = {
        toolType: base.toolType,
        tangential: base.tangential,
        offsetMm: base.offsetMm,
        unwind: base.unwind,
        cornerAngleDeg: base.cornerAngleDeg,
        minRadiusMm: base.minRadiusMm,
        liftHeight: base.liftHeight,
        requiredPeripheralTypes: base.requiredPeripheralTypes,
        toolOffset: base.toolOffset,
    };
    if (base.path !== undefined) merged.path = base.path;
    if (base.z !== undefined) merged.z = base.z;
    if (base.slotOffsets !== undefined) merged.slotOffsets = base.slotOffsets;

    if (o.tangential !== undefined) merged.tangential = o.tangential;
    if (o.offsetMm !== undefined) merged.offsetMm = o.offsetMm;
    if (o.unwind !== undefined) merged.unwind = o.unwind;
    if (o.cornerAngleDeg !== undefined) merged.cornerAngleDeg = o.cornerAngleDeg;
    if (o.minRadiusMm !== undefined) merged.minRadiusMm = o.minRadiusMm;
    // Engage targets: merge feed/accel over any preset value.
    if (o.path !== undefined) merged.path = opTarget(o.path, base.path ?? {});
    if (o.z !== undefined) merged.z = opTarget(o.z, base.z ?? {});
    if (o.liftHeight !== undefined) merged.liftHeight = o.liftHeight;
    if (o.toolOffset !== undefined) merged.toolOffset = o.toolOffset;
    if (o.slotOffsets !== undefined) merged.slotOffsets = o.slotOffsets;

    return toolProfile(name, merged as Partial<Omit<ToolProfile, "name">>);
}

function err(msg: string): ConfigResult {
    return { ok: false, errors: [msg] };
}
