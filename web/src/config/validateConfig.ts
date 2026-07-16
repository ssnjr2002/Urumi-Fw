/**
 * validateConfig.ts — scoped feed/accel validation.
 *
 * Deliberately narrow (see docs/feed_accel_value_model.md § Validation):
 * this pass checks ONLY the feed/accel value model, not the config as a whole.
 * Duplicate node ids, defaultHead range, node-type agreement, toolOffset
 * tolerance — all out of scope, a separate effort.
 *
 * Rules:
 *   error   — any feed or accel < 0 (ceilings and targets alike).
 *   warning — X or Y axis with a 0 (uncapped) maxFeed/maxAccel; a target feed
 *             or accel that exceeds its participating axis ceiling (harmless —
 *             it is clamped — but almost always a misunderstanding).
 *
 * Structural presence (fCpu, stepsPerUnit, a head with tool+z+a) is enforced by
 * the loader; this pass assumes a well-formed PipelineConfig.
 */

import type {
    AxisConfig,
    MachineConfig,
    OpTarget,
    PipelineConfig,
    ToolProfile,
} from "./config.js";
import { resolvedAxes } from "./config.js";

export interface ValidationResult {
    readonly errors: readonly string[];
    readonly warnings: readonly string[];
}

export function validateConfig(config: PipelineConfig): ValidationResult {
    const errors: string[] = [];
    const warnings: string[] = [];
    const { machine } = config;

    // ── axis ceilings: non-negative; X/Y must be capped ──────────────────────
    const namedAxes: readonly [string, AxisConfig][] = [
        ["machine.x", machine.x],
        ["machine.y", machine.y],
        ...machine.heads.flatMap((h, i): [string, AxisConfig][] => [
            [`heads[${i}].z`, h.z],
            [`heads[${i}].a`, h.a],
        ]),
    ];
    for (const [path, ax] of namedAxes) {
        if (ax.maxFeed < 0) errors.push(`${path}.maxFeed: must be >= 0`);
        if (ax.maxAccel < 0) errors.push(`${path}.maxAccel: must be >= 0`);
    }
    for (const [path, ax] of [["machine.x", machine.x], ["machine.y", machine.y]] as const) {
        if (ax.maxFeed === 0) warnings.push(`${path}.maxFeed: 0 (uncapped) — X/Y should have a physical ceiling`);
        if (ax.maxAccel === 0) warnings.push(`${path}.maxAccel: 0 (uncapped) — X/Y should have a physical ceiling`);
    }

    // ── target non-negativity ─────────────────────────────────────────────────
    const namedTargets: readonly [string, OpTarget][] = [
        ["machine.path", machine.path],
        ["machine.rapid", machine.rapid],
        ["machine.z", machine.z],
        ["machine.slew", machine.slew],
        ...Object.entries(config.toolProfiles).flatMap(
            ([name, t]: [string, ToolProfile]): [string, OpTarget][] => [
                ...(t.path ? [[`tools.${name}.path`, t.path] as [string, OpTarget]] : []),
                ...(t.z ? [[`tools.${name}.z`, t.z] as [string, OpTarget]] : []),
            ],
        ),
    ];
    for (const [path, t] of namedTargets) {
        if (t.feed !== undefined && t.feed < 0) errors.push(`${path}.feed: must be >= 0`);
        if (t.accel !== undefined && t.accel < 0) errors.push(`${path}.accel: must be >= 0`);
    }

    // ── over-ceiling warnings (target > participating-axis ceiling) ───────────
    const axes = resolvedAxes(machine);
    const xyFeed = minPositive(machine.x.maxFeed, machine.y.maxFeed);
    const xyAccel = minPositive(machine.x.maxAccel, machine.y.maxAccel);

    overCeiling(warnings, "machine.rapid", machine.rapid, xyFeed, xyAccel);
    overCeiling(warnings, "machine.slew", machine.slew, axes.a.maxFeed, axes.a.maxAccel);
    // path (XY) and z resolve per tool over the machine baseline.
    for (const [name, t] of Object.entries(config.toolProfiles)) {
        const path = resolve(t.path, machine.path);
        const z = resolve(t.z, machine.z);
        overCeiling(warnings, `tools.${name}.path`, path, xyFeed, xyAccel);
        overCeiling(warnings, `tools.${name}.z`, z, axes.z.maxFeed, axes.z.maxAccel);
    }

    return { errors, warnings };
}

/** min of two ceilings, ignoring 0 (uncapped). 0 if both uncapped. */
function minPositive(a: number, b: number): number {
    const cands = [a, b].filter((v) => v > 0);
    return cands.length ? Math.min(...cands) : 0;
}

function resolve(tool: OpTarget | undefined, machine: OpTarget): OpTarget {
    return {
        feed: tool?.feed ?? machine.feed,
        accel: tool?.accel ?? machine.accel,
    };
}

function overCeiling(
    warnings: string[],
    path: string,
    t: OpTarget,
    feedCeil: number,
    accelCeil: number,
): void {
    if (feedCeil > 0 && t.feed !== undefined && t.feed > feedCeil) {
        warnings.push(`${path}.feed ${t.feed} exceeds axis ceiling ${feedCeil} (clamped)`);
    }
    if (accelCeil > 0 && t.accel !== undefined && t.accel > accelCeil) {
        warnings.push(`${path}.accel ${t.accel} exceeds axis ceiling ${accelCeil} (clamped)`);
    }
}
