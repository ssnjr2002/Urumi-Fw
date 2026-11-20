/**
 * validate.ts — the second pass: do a well-formed config's values make sense?
 *
 * A rule is a function over the whole config, listed in RULES; each is
 * independent and tested on its own (validate.test.ts).
 *
 * error   — the config is wrong; loadConfig refuses it.
 * warning — legal but probably not meant; shown, never enforced (a target
 *           above its axis's ceiling, which is clamped).
 */

import type {
    AxisConfig,
    MachineConfig,
    OpTarget,
    ControllerConfig,
} from "../schema.js";
import { NodeType } from "../schema.js";
import { TOOL_PROFILES_BY_TYPE } from "../tools.js";
import { requiredAxes, resolveTargets } from "../resolve.js";
import { runtimeFieldError, runtimeMisplaced, runtimeValues } from "../runtime.js";


export interface ValidationResult {
    readonly errors: readonly string[];
    readonly warnings: readonly string[];
}

interface Issue {
    readonly level: "error" | "warning";
    readonly message: string;
}

type Rule = (config: ControllerConfig) => Issue[];

const error = (message: string): Issue => ({ level: "error", message });
const warn = (message: string): Issue => ({ level: "warning", message });

// ── rules ─────────────────────────────────────────────────────────────────────

/** Every axis, labelled with the config path an operator would edit. */
function namedAxes(machine: MachineConfig): readonly [string, AxisConfig][] {
    return [
        ["machine.x", machine.x],
        ["machine.y", machine.y],
        ...machine.heads.flatMap((h, i): [string, AxisConfig][] => [
            [`machine.heads[${i}].z`, h.z],
            [`machine.heads[${i}].a`, h.a],
        ]),
    ];
}

/** The controller can edit these fields; the table holds their ranges for both sides. */
const runtimeFieldsInRange: Rule = (config) =>
    runtimeValues(config).flatMap(({ field, label, value }) => {
        if (value === undefined) return [];
        const problem = runtimeFieldError(field, value);
        return problem === null ? [] : [error(`${label}: ${problem}`)];
    });

const nonNegativeSlew: Rule = (config) =>
    (["feed", "accel"] as const)
        .filter((k) => (config.machine.slew[k] ?? 0) < 0)
        .map((k) => error(`machine.slew.${k}: must be >= 0`));

/**
 * X/Y with a 0 (uncapped) ceiling is legal but reckless: nothing then bounds
 * gantry speed except the tool's own target, so a tool with a high feed will
 * command whatever it likes.
 */
const xyMustBeCapped: Rule = ({ machine }) =>
    ([["machine.x", machine.x], ["machine.y", machine.y]] as const).flatMap(([path, ax]) => [
        ...(ax.maxFeed === 0
            ? [warn(`${path}.maxFeed: 0 (uncapped) — X/Y should have a physical ceiling`)]
            : []),
        ...(ax.maxAccel === 0
            ? [warn(`${path}.maxAccel: 0 (uncapped) — X/Y should have a physical ceiling`)]
            : []),
    ]);

/** A target above the ceiling of an axis it drives is clamped — say so. */
const targetsUnderCeilings: Rule = (config) => {
    const { machine } = config;
    const xyFeed = minPositive(machine.x.maxFeed, machine.y.maxFeed);

    const issues: Issue[] = [
        ...feedOver("machine.feed.travelDefault", machine.feed.travelDefault, xyFeed),
    ];
    for (const [name, t] of Object.entries(config.toolProfiles)) {
        issues.push(...feedOver(`tools.${name}.cutFeed`, resolveTargets(machine, t).cutFeed, xyFeed));
    }

    // Z/A ceilings come from the default head, which may not exist: rules don't
    // assume others passed, and resolvedAxes() would throw on it.
    const head = machine.heads[machine.defaultHead];
    if (head === undefined) return issues;

    issues.push(...overCeiling("machine.slew", machine.slew, head.a.maxFeed, head.a.maxAccel));
    for (const [name, t] of Object.entries(config.toolProfiles)) {
        issues.push(...feedOver(`tools.${name}.plungeFeed`, resolveTargets(machine, t).plungeFeed, head.z.maxFeed));
    }
    return issues;
};

/**
 * An RS485 address names one node: two axes on one id step together, and
 * nothing downstream can tell.
 */
const uniqueNodeIds: Rule = ({ machine }) => {
    const owners = new Map<number, string[]>();
    const claim = (id: number, path: string) =>
        owners.set(id, [...(owners.get(id) ?? []), path]);

    for (const [path, ax] of namedAxes(machine)) claim(ax.node.id, path);
    machine.peripherals.forEach((p, i) => claim(p.id, `machine.peripherals[${i}]`));

    return [...owners.entries()]
        .filter(([, paths]) => paths.length > 1)
        .map(([id, paths]) =>
            error(`node id ${id} claimed by ${paths.join(", ")} — ids must be unique`),
        );
};

/** A tool id is a u8 on the wire, 0 meaning no tool, and names one tool. */
const toolIdsUnique: Rule = ({ toolProfiles }) => {
    const seen = new Map<number, string>();
    return Object.entries(toolProfiles).flatMap(([name, t]) => {
        if (t.id === undefined) return [];
        if (!Number.isInteger(t.id) || t.id < 1 || t.id > 255) {
            return [error(`tools.${name}.id: must be an integer 1..255 (got ${t.id})`)];
        }
        const owner = seen.get(t.id);
        if (owner !== undefined) return [error(`tools.${name}.id: ${t.id} is also ${owner}'s`)];
        seen.set(t.id, name);
        return [];
    });
};

/** A tangential tool's A fields, and chambers only on a tool that does not steer. */
const toolAFields: Rule = ({ toolProfiles }) =>
    Object.entries(toolProfiles).flatMap(([name, t]) => [
        ...(Number.isInteger(t.periodCount) && t.periodCount >= 1 && t.periodCount <= 255
            ? [] : [error(`tools.${name}.periodCount: must be a whole number 1..255 (got ${t.periodCount})`)]),
        ...(!t.tangential && t.periodCount !== 1
            ? [error(`tools.${name}.periodCount: only a tangential tool has one`)] : []),
        ...(t.aOffsetDeg === undefined || t.tangential
            ? [] : [error(`tools.${name}.aOffsetDeg: tangential tools only`)]),
        ...(t.tangential || t.offsetMm === 0
            ? [] : [error(`tools.${name}.offsetMm: 0 unless tangential (got ${t.offsetMm})`)]),
        ...(t.tangential && t.chamberOffsets !== undefined
            ? [error(`tools.${name}.chamberOffsets: a tangential tool has no chambers`)] : []),
    ]);

/** The firmware's CFG_MAX_HEADS; STATUS_RSP carries a tool for each. */
export const MAX_HEADS = 2;

const headCountSupported: Rule = ({ machine }) => {
    const n = machine.heads.length;
    return n >= 1 && n <= MAX_HEADS
        ? []
        : [error(`machine.heads: ${n} head(s); the controller supports 1 to ${MAX_HEADS}`)];
};

const defaultHeadInRange: Rule = ({ machine }) => {
    const n = machine.heads.length;
    const i = machine.defaultHead;
    return Number.isInteger(i) && i >= 0 && i < n
        ? []
        : [error(`machine.defaultHead: ${i} is out of range (machine has ${n} head(s))`)];
};

/** A table field set on an axis its row excludes (unlockRange off A). */
const runtimeFieldsPlaced: Rule = (config) => runtimeMisplaced(config).map((m) => error(m));

/**
 * A head accepting a tool whose Z or A node is absent: the head is off the bus,
 * or wired without that axis. A warning: headsAccepting leaves such a head out,
 * so no work is planned onto it until the node is present again.
 */
const headsHaveAxesForAccepted: Rule = ({ machine }) =>
    machine.heads.flatMap((h, i) =>
        h.accepts.flatMap((type) => {
            const p = TOOL_PROFILES_BY_TYPE[type];
            if (!p) return [];
            const req = requiredAxes(p);
            return [
                ...(req.a && !h.a.node.present
                    ? [warn(`machine.heads[${i}]: accepts '${p.name}', which steers A, ` +
                            "but that head's A node is not present; it takes none until it is")]
                    : []),
                ...(req.z && !h.z.node.present
                    ? [warn(`machine.heads[${i}]: accepts '${p.name}', which needs Z, ` +
                            "but that head's Z node is not present; it takes none until it is")]
                    : []),
            ];
        }),
    );

/**
 * A tool no head accepts can never be scheduled. A warning: a machine need not
 * hold every preset, and this names the tool at load rather than at the bake.
 */
const everyToolHasAHead: Rule = ({ machine, toolProfiles }) => {
    const accepted = new Set(machine.heads.flatMap((h) => [...h.accepts]));
    return Object.entries(toolProfiles)
        .filter(([, p]) => !accepted.has(p.toolType))
        .map(([name]) => warn(`tools.${name}: no head accepts it — it can never be scheduled`));
};

/** A socket that accepts nothing is declared and unusable. */
const headsAcceptSomething: Rule = ({ machine }) =>
    machine.heads.flatMap((h, i) =>
        h.accepts.length === 0
            ? [warn(`machine.heads[${i}]: accepts nothing — no job can use this head`)]
            : [],
    );

/**
 * A power block names a node that can switch the tool: a present KNIFE_OSC
 * peripheral, or the laser's STEPPER node (a bench stand-in; only a controller
 * built with ACCEPT_LASER_AS_POWER_TOOL accepts it). Duty limits come as a
 * set; load.ts fills a missing number with 0, so a partial block is named here.
 *
 * minOnS >= maxOnS would leave no on-time before a reset is due.
 */
const powerCoherent: Rule = (config) =>
    Object.entries(config.toolProfiles).flatMap(([name, t]) => {
        const pw = t.power;
        if (!pw) return [];
        const p = `tools.${name}.power`;
        const out: Issue[] = [];
        const m = config.machine;
        const { id, type } = pw.node;
        const knife = type === NodeType.KNIFE_OSC &&
            m.peripherals.some((n) => n.id === id && n.present && n.type === type);
        const laser = type === NodeType.STEPPER && id === m.laser?.node;
        if (!knife && !laser) {
            out.push(error(
                `${p}.node: {id: ${id}, type: ${type}} is not a present KNIFE_OSC ` +
                "peripheral or the laser's STEPPER node",
            ));
        }
        if (!(pw.settleS >= 0)) out.push(error(`${p}.settleS: must be >= 0 (got ${pw.settleS})`));
        const d = pw.dutyLimits;
        if (!d) return out;
        const q = `${p}.dutyLimits`;
        if (!(d.maxOnS > 0)) out.push(error(`${q}.maxOnS: must be > 0 (got ${d.maxOnS})`));
        if (!(d.minOnS > 0)) out.push(error(`${q}.minOnS: must be > 0 (got ${d.minOnS})`));
        if (!(d.dwellS > 0)) out.push(error(`${q}.dwellS: must be > 0 (got ${d.dwellS})`));
        if (d.maxOnS > 0 && d.minOnS > 0 && d.minOnS >= d.maxOnS) {
            out.push(error(`${q}: minOnS (${d.minOnS}) must be < maxOnS (${d.maxOnS})`));
        }
        return out;
    });

/**
 * Homing recipes that would misbehave on the machine rather than in the parser.
 *
 * Only the fields the host types are checked; the controller's decoder checks
 * the rest.
 */
const homingCoherent: Rule = ({ machine }) =>
    namedAxes(machine).flatMap(([path, ax]) => {
        const h = ax.homing;
        if (h === undefined) return [];
        const p = `${path}.homing`;
        const issues: Issue[] = [];

        if (h.kind === "rotary") {
            if (ax.maxFeed > 0 && h.sweepFeed > ax.maxFeed) {
                issues.push(error(`${p}.sweepFeed: exceeds ${path}.maxFeed (${ax.maxFeed})`));
            }
            // A rotary axis has no ends: its soft limit is unbounded (0) or a deliberate
            // restriction, and homing spins freely either way.
            if (!ax.rotary) {
                issues.push(error(
                    `${p}: rotary homing on ${path}, which is not marked rotary`,
                ));
            }
            return issues;
        }

        // The slow leg sets repeatability; not slower, it is no re-approach.
        if (h.latchFeed >= h.seekFeed) {
            issues.push(error(`${p}.latchFeed: must be < seekFeed (${h.seekFeed})`));
        }
        // The seek runs at seekFeed for nearly its whole length, so the axis
        // ceiling applies to it exactly as it does to a job move.
        if (ax.maxFeed > 0 && h.seekFeed > ax.maxFeed) {
            issues.push(error(`${p}.seekFeed: exceeds ${path}.maxFeed (${ax.maxFeed})`));
        }
        return issues;
    });


/**
 * The probe block, on homingCoherent's terms: reject only what has a physical
 * consequence. Every feed is a divisor into a step interval, so a zero is an
 * infinite interval; every distance is a leg that either happens or does not.
 */
const probeCoherent: Rule = ({ machine }) =>
    namedAxes(machine).flatMap(([path, ax]) => {
        const pr = ax.probe;
        if (pr === undefined) return [];
        const p = `${path}.probe`;
        const issues: Issue[] = [];

        for (const k of ["pullInFeed", "seekFeed", "latchFeed", "probeTravel",
                         "replyDeadlineUs"] as const) {
            if (pr[k] <= 0) issues.push(error(`${p}.${k}: must be > 0`));
        }
        if (pr.rampSteps < 0) issues.push(error(`${p}.rampSteps: must be >= 0`));

        if (pr.pullInFeed > pr.seekFeed) {
            issues.push(error(`${p}.pullInFeed: must be <= seekFeed (${pr.seekFeed})`));
        }
        if (ax.maxFeed > 0 && pr.seekFeed > ax.maxFeed) {
            issues.push(error(`${p}.seekFeed: exceeds ${path}.maxFeed (${ax.maxFeed})`));
        }
        // The latch measures: faster than the seek means the legs were swapped.
        if (pr.latchFeed > pr.seekFeed) {
            issues.push(error(
                `${p}.latchFeed (${pr.latchFeed}) > seekFeed (${pr.seekFeed}) — ` +
                `the latch is the leg that measures and must be the slow one`,
            ));
        }

        // A backoff inside the switch's hysteresis never leaves it; only the
        // degenerate case is knowable here.
        if (pr.backoffMm <= 0) issues.push(error(`${p}.backoffMm: must be > 0`));

        // Parking on the switch makes the next probe refuse with ALREADY_OPEN.
        if (pr.parkMm <= 0) issues.push(error(`${p}.parkMm: must be > 0`));

        // Under one step, the poll divisor clamps to 1 and the seek runs at latch
        // cadence; say why it is slow.
        if (pr.seekOvertravelMm < 0) {
            issues.push(error(`${p}.seekOvertravelMm: must be >= 0`));
        } else if (pr.seekOvertravelMm * ax.stepsPerUnit < 1) {
            issues.push(warn(
                `${p}.seekOvertravelMm (${pr.seekOvertravelMm}) is under one step ` +
                `at ${ax.stepsPerUnit} steps/mm — the seek will poll every step ` +
                `and run at latch speed`,
            ));
        }

        // The budget is a search window below the start, compared against nothing.
        return issues;
    });

/**
 * Something must sit at (0, 0): the laser when defined, else a head. The laser
 * is switched through its node, so that must be a configured node. Reach of
 * the probe switches and stored positions is the controller's check.
 */
const hasAnchor: Rule = ({ machine }) => {
    const issues: Issue[] = [];
    if (machine.laser) {
        const ids = [machine.x, machine.y, ...machine.heads.flatMap((h) => [h.z, h.a])]
            .map((a) => a.node.id)
            .concat(machine.peripherals.map((p) => p.id));
        if (!ids.includes(machine.laser.node)) {
            issues.push(error(`machine.laser.node ${machine.laser.node}: not a configured node`));
        }
    }
    if (!machine.laser && !machine.heads.some((h) => h.xOffset === 0 && h.yOffset === 0)) {
        issues.push(error("no anchor: define a laser or place a head at (0, 0)"));
    }
    return issues;
};

/** It may reach past the axes: a border can lie beyond the laser's reach. */
const workAreaCoherent: Rule = ({ machine }) => {
    const w = machine.workArea;
    if (!w) return [];
    const finite = [w.xMin, w.xMax, w.yMin, w.yMax].every(Number.isFinite);
    return finite && w.xMin < w.xMax && w.yMin < w.yMax
        ? []
        : [error("machine.workArea: needs finite bounds with xMin < xMax and yMin < yMax")];
};

const RULES: readonly Rule[] = [
    hasAnchor,
    workAreaCoherent,
    homingCoherent,
    probeCoherent,
    runtimeFieldsInRange,
    nonNegativeSlew,
    xyMustBeCapped,
    targetsUnderCeilings,
    uniqueNodeIds,
    headCountSupported,
    defaultHeadInRange,
    runtimeFieldsPlaced,
    toolIdsUnique,
    toolAFields,
    headsHaveAxesForAccepted,
    headsAcceptSomething,
    everyToolHasAHead,
    powerCoherent,
];

// ── entry ─────────────────────────────────────────────────────────────────────

export function validateConfig(config: ControllerConfig): ValidationResult {
    const issues = RULES.flatMap((rule) => rule(config));
    return {
        errors: issues.filter((i) => i.level === "error").map((i) => i.message),
        warnings: issues.filter((i) => i.level === "warning").map((i) => i.message),
    };
}

// ── helpers ───────────────────────────────────────────────────────────────────

/** min of two ceilings, ignoring 0 (uncapped). 0 if both uncapped. */
function minPositive(a: number, b: number): number {
    const capped = [a, b].filter((v) => v > 0);
    return capped.length ? Math.min(...capped) : 0;
}

function feedOver(path: string, feed: number, ceil: number): Issue[] {
    return ceil > 0 && feed > ceil ? [warn(`${path} ${feed} exceeds axis ceiling ${ceil} (clamped)`)] : [];
}

function overCeiling(path: string, t: OpTarget, feedCeil: number, accelCeil: number): Issue[] {
    return [
        ...(feedCeil > 0 && t.feed !== undefined && t.feed > feedCeil
            ? [warn(`${path}.feed ${t.feed} exceeds axis ceiling ${feedCeil} (clamped)`)]
            : []),
        ...(accelCeil > 0 && t.accel !== undefined && t.accel > accelCeil
            ? [warn(`${path}.accel ${t.accel} exceeds axis ceiling ${accelCeil} (clamped)`)]
            : []),
    ];
}
