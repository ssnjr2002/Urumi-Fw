/**
 * validate.ts — semantic validation. The second of the two passes.
 *
 * load.ts already proved the config is well-FORMED; this pass asks whether the
 * values make SENSE. It assumes a structurally valid PipelineConfig and never
 * inspects raw JSON.
 *
 * Adding a rule = write a function, append it to RULES. Nothing else. Each rule
 * is independent and sees the whole config, so rules stay individually testable
 * (one describe() per rule in validate.test.ts) and the pass has no ordering
 * subtleties.
 *
 * error   — the config is wrong; loadConfig refuses it.
 * warning — legal but probably not what the operator meant; surfaced, never
 *           enforced. A target above its axis ceiling is the canonical case:
 *           harmless (it gets clamped) but almost always a misunderstanding.
 */

import type {
    AxisConfig,
    MachineConfig,
    OpTarget,
    PipelineConfig,
    ToolProfile,
} from "../schema.js";
import { NodeType } from "../schema.js";
import { TOOL_PROFILES_BY_TYPE } from "../tools.js";
import { requiredAxes, resolveTargets } from "../resolve.js";


export interface ValidationResult {
    readonly errors: readonly string[];
    readonly warnings: readonly string[];
}

interface Issue {
    readonly level: "error" | "warning";
    readonly message: string;
}

type Rule = (config: PipelineConfig) => Issue[];

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

/** Ceilings are magnitudes; a negative one is meaningless, not merely unwise. */
const nonNegativeCeilings: Rule = ({ machine }) =>
    namedAxes(machine).flatMap(([path, ax]) => [
        ...(ax.maxFeed < 0 ? [error(`${path}.maxFeed: must be >= 0`)] : []),
        ...(ax.maxAccel < 0 ? [error(`${path}.maxAccel: must be >= 0`)] : []),
    ]);

const nonNegativeTargets: Rule = (config) => [
    ...(["feed", "accel"] as const)
        .filter((k) => (config.machine.slew[k] ?? 0) < 0)
        .map((k) => error(`machine.slew.${k}: must be >= 0`)),
    ...Object.entries(config.toolProfiles).flatMap(([name, t]: [string, ToolProfile]) =>
        (["cutFeed", "plungeFeed"] as const)
            .filter((k) => (t[k] ?? 0) < 0)
            .map((k) => error(`tools.${name}.${k}: must be >= 0`)),
    ),
];

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
    const xyAccel = minPositive(machine.x.maxAccel, machine.y.maxAccel);

    const issues: Issue[] = [
        ...feedOver("machine.feed.travelDefault", machine.feed.travelDefault, xyFeed),
    ];
    for (const [name, t] of Object.entries(config.toolProfiles)) {
        issues.push(...feedOver(`tools.${name}.cutFeed`, resolveTargets(machine, t).cutFeed, xyFeed));
    }

    // Z/A ceilings live on the DEFAULT HEAD, which may not exist — every rule
    // runs on the same config, so a rule must never assume another rule passed.
    // resolvedAxes() asserts heads[defaultHead] is present; calling it here
    // would throw on the very config defaultHeadInRange is about to reject,
    // turning a reported error into a crash inside loadConfig.
    const head = machine.heads[machine.defaultHead];
    if (head === undefined) return issues;

    issues.push(...overCeiling("machine.slew", machine.slew, head.a.maxFeed, head.a.maxAccel));
    for (const [name, t] of Object.entries(config.toolProfiles)) {
        issues.push(...feedOver(`tools.${name}.plungeFeed`, resolveTargets(machine, t).plungeFeed, head.z.maxFeed));
    }
    return issues;
};

/**
 * An RS485 address identifies exactly one node. Two axes sharing an id means
 * both step together — a wiring-level fault no downstream stage can detect,
 * so it has to be caught here or not at all.
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

const clearanceNonNegative: Rule = ({ machine }) =>
    Number.isFinite(machine.zClearance) && machine.zClearance >= 0
        ? []
        : [error(`machine.zClearance: must be >= 0 (got ${machine.zClearance})`)];

const bufferPositive: Rule = ({ machine }) =>
    (["queueTime", "idleTime"] as const)
        .filter((k) => !(Number.isFinite(machine.buffer[k]) && machine.buffer[k] > 0))
        .map((k) => error(`machine.buffer.${k}: must be > 0 (got ${machine.buffer[k]})`));

const feedAtLeastOne: Rule = ({ machine }) =>
    (["cutFallback", "travelDefault", "plungeFallback", "liftFeed"] as const)
        .filter((k) => !(Number.isFinite(machine.feed[k]) && machine.feed[k] >= 1))
        .map((k) => error(`machine.feed.${k}: must be >= 1 (got ${machine.feed[k]})`));

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
        ...(t.cornerAngleDeg >= 0 && t.cornerAngleDeg <= 180
            ? [] : [error(`tools.${name}.cornerAngleDeg: must be 0..180 (got ${t.cornerAngleDeg})`)]),
        ...(t.aMaxDeg === undefined || (Number.isFinite(t.aMaxDeg) && t.aMaxDeg >= 0)
            ? [] : [error(`tools.${name}.aMaxDeg: must be >= 0 (got ${t.aMaxDeg})`)]),
        ...(t.aOffsetDeg === undefined || (Number.isFinite(t.aOffsetDeg) && t.tangential)
            ? [] : [error(`tools.${name}.aOffsetDeg: a finite angle, tangential tools only`)]),
        ...(Number.isFinite(t.offsetMm) && t.offsetMm >= 0 && (t.tangential || t.offsetMm === 0)
            ? [] : [error(`tools.${name}.offsetMm: >= 0 mm, and 0 unless tangential (got ${t.offsetMm})`)]),
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

/**
 * A head cannot accept a tool it has no axes for.
 *
 * `accepts` is a physical claim, and the scheduler treats it as one: a head
 * listing the knife WILL be given knife blocks, and Z/A come from that head. A
 * tangential tool on a head with no A node is a job that bakes cleanly and then
 * cannot steer, so the claim has to be refused at load, not discovered later.
 */
const headsHaveAxesForAccepted: Rule = ({ machine }) =>
    machine.heads.flatMap((h, i) =>
        h.accepts.flatMap((type) => {
            const p = TOOL_PROFILES_BY_TYPE[type];
            if (!p) return [];
            const req = requiredAxes(p);
            return [
                ...(req.a && !h.a.node.present
                    ? [error(`machine.heads[${i}]: accepts '${p.name}', which steers A, ` +
                             "but that head's A node is not present")]
                    : []),
                ...(req.z && !h.z.node.present
                    ? [error(`machine.heads[${i}]: accepts '${p.name}', which needs Z, ` +
                             "but that head's Z node is not present")]
                    : []),
            ];
        }),
    );

/**
 * A tool no head accepts can never be scheduled — every job using it fails at
 * the bake. A warning, not an error: a machine legitimately need not be able to
 * hold every preset the code ships, and only the presets a job actually uses
 * matter. Naming it here is what turns "why does this SVG refuse to bake" into
 * one line at load.
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
 * peripheral, or the laser's STEPPER node (a bench stand-in; only a Pico built with
 * ACCEPT_LASER_AS_POWER_TOOL accepts it). Duty limits are only meaningful as a
 * complete set; load.ts fills a missing number with 0 rather than inventing
 * one, so a partial block arrives here as a zero and is named precisely.
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
 * These are semantic checks, so they live here and not in load.ts: every one of
 * them parses fine and only goes wrong when a motor turns. Only the fields the
 * host types are checked; the Pico's decoder checks the rest.
 */
const homingCoherent: Rule = ({ machine }) =>
    namedAxes(machine).flatMap(([path, ax]) => {
        const h = ax.homing;
        if (h === undefined) return [];
        const p = `${path}.homing`;
        const issues: Issue[] = [];

        if (h.kind === "rotary") {
            // Same shape of check, different physics. Feeds are divisors either
            // way, so a zero is still an infinite step interval.
            for (const k of ["sweepFeed", "budgetRevs", "toleranceDeg"] as const) {
                if (h[k] <= 0) issues.push(error(`${p}.${k}: must be > 0`));
            }
            if (h.rampSteps < 0) issues.push(error(`${p}.rampSteps: must be >= 0`));
            if (ax.maxFeed > 0 && h.sweepFeed > ax.maxFeed) {
                issues.push(error(`${p}.sweepFeed: exceeds ${path}.maxFeed (${ax.maxFeed})`));
            }
            // The measured floor, not a round number. A sweep needs three full
            // laps in the worst starting phase plus its post-roll; under this a
            // healthy axis reports `notfound`, which reads as a dead sensor and
            // sends an operator to the wiring.
            if (h.budgetRevs < 3.2) {
                issues.push(error(
                    `${p}.budgetRevs (${h.budgetRevs}): must be >= 3.2 — a sweep that ` +
                    `starts just past the index needs three full laps plus post-roll`,
                ));
            }
            // A rotary axis has no ends, so a soft-limit envelope on one is
            // either unbounded (0) or a deliberate restriction. Homing spins
            // freely regardless, which is worth saying once here.
            if (!ax.rotary) {
                issues.push(error(
                    `${p}: rotary homing on ${path}, which is not marked rotary`,
                ));
            }
            return issues;
        }

        // Feeds are divisors; a zero produces an infinite step interval.
        for (const k of ["seekFeed", "latchFeed"] as const) {
            if (h[k] <= 0) issues.push(error(`${p}.${k}: must be > 0`));
        }
        if (h.rampSteps < 0) issues.push(error(`${p}.rampSteps: must be >= 0`));

        // The slow leg is what sets repeatability; if it is not slower than the
        // fast one, leg 3 is not a re-approach and the whole two-pass structure
        // buys nothing.
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
        // The latch is the leg that measures, so a latch faster than the seek is
        // not a small mistake -- it means the two legs were swapped, and the
        // number reported as a measurement came from the fast one.
        if (pr.latchFeed > pr.seekFeed) {
            issues.push(error(
                `${p}.latchFeed (${pr.latchFeed}) > seekFeed (${pr.seekFeed}) — ` +
                `the latch is the leg that measures and must be the slow one`,
            ));
        }

        // A backoff shorter than the switch's release hysteresis never leaves the
        // switch, and the latch that follows then starts already tripped. Same
        // failure homing's backoffMm has; the hysteresis is not knowable here, so
        // only the degenerate case is caught.
        if (pr.backoffMm <= 0) issues.push(error(`${p}.backoffMm: must be > 0`));

        // Parking ON the switch leaves the next probe's arm check reading open,
        // which is refused as ALREADY_OPEN -- a healthy machine that cannot probe
        // twice in a row.
        if (pr.parkMm <= 0) issues.push(error(`${p}.parkMm: must be > 0`));

        // Overtravel converts to a poll divisor of `floor(mm x stepsPerUnit)`,
        // which is 0 below one step -- harmless (it clamps to 1, polling every
        // step) but it means the seek silently runs at latch cadence, so say so
        // rather than let someone wonder why the seek is slow.
        if (pr.seekOvertravelMm < 0) {
            issues.push(error(`${p}.seekOvertravelMm: must be >= 0`));
        } else if (pr.seekOvertravelMm * ax.stepsPerUnit < 1) {
            issues.push(warn(
                `${p}.seekOvertravelMm (${pr.seekOvertravelMm}) is under one step ` +
                `at ${ax.stepsPerUnit} steps/mm — the seek will poll every step ` +
                `and run at latch speed`,
            ));
        }

        // The budget is a search window below the start, not a frame dimension,
        // so it is NOT compared against maxTravel the way hardTravel is. It is
        // compared against nothing: how far a tool may protrude is not something
        // config knows.
        return issues;
    });

/**
 * Something must sit at (0, 0): the laser when defined, else a head. The laser
 * is switched through its node, so that must be a configured node. Reach of
 * the probe switches and stored positions is the Pico's check.
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

const RULES: readonly Rule[] = [
    hasAnchor,
    homingCoherent,
    probeCoherent,
    nonNegativeCeilings,
    nonNegativeTargets,
    xyMustBeCapped,
    targetsUnderCeilings,
    uniqueNodeIds,
    headCountSupported,
    defaultHeadInRange,
    clearanceNonNegative,
    bufferPositive,
    feedAtLeastOne,
    toolIdsUnique,
    toolAFields,
    headsHaveAxesForAccepted,
    headsAcceptSomething,
    everyToolHasAHead,
    powerCoherent,
];

// ── entry ─────────────────────────────────────────────────────────────────────

export function validateConfig(config: PipelineConfig): ValidationResult {
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
