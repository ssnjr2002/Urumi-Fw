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
import { TOOL_PROFILES_BY_TYPE } from "../tools.js";
import { requiredAxes } from "../resolve.js";


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
            [`heads[${i}].z`, h.z],
            [`heads[${i}].a`, h.a],
        ]),
    ];
}

/** Ceilings are magnitudes; a negative one is meaningless, not merely unwise. */
const nonNegativeCeilings: Rule = ({ machine }) =>
    namedAxes(machine).flatMap(([path, ax]) => [
        ...(ax.maxFeed < 0 ? [error(`${path}.maxFeed: must be >= 0`)] : []),
        ...(ax.maxAccel < 0 ? [error(`${path}.maxAccel: must be >= 0`)] : []),
    ]);

const nonNegativeTargets: Rule = (config) =>
    namedTargets(config).flatMap(([path, t]) => [
        ...(t.feed !== undefined && t.feed < 0 ? [error(`${path}.feed: must be >= 0`)] : []),
        ...(t.accel !== undefined && t.accel < 0 ? [error(`${path}.accel: must be >= 0`)] : []),
    ]);

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
        ...overCeiling("machine.rapid", machine.rapid, xyFeed, xyAccel),
    ];
    for (const [name, t] of Object.entries(config.toolProfiles)) {
        issues.push(
            ...overCeiling(`tools.${name}.path`, inherit(t.path, machine.path), xyFeed, xyAccel),
        );
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
        issues.push(
            ...overCeiling(`tools.${name}.z`, inherit(t.z, machine.z), head.z.maxFeed, head.z.maxAccel),
        );
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
    machine.peripherals.forEach((p, i) => claim(p.id, `peripherals[${i}]`));

    return [...owners.entries()]
        .filter(([, paths]) => paths.length > 1)
        .map(([id, paths]) =>
            error(`node id ${id} claimed by ${paths.join(", ")} — ids must be unique`),
        );
};

const defaultHeadInRange: Rule = ({ machine }) => {
    const n = machine.heads.length;
    const i = machine.defaultHead;
    return Number.isInteger(i) && i >= 0 && i < n
        ? []
        : [error(`defaultHead: ${i} is out of range (machine has ${n} head(s))`)];
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
                    ? [error(`heads[${i}]: accepts '${p.name}', which steers A, ` +
                             "but that head's A node is not present")]
                    : []),
                ...(req.z && !h.z.node.present
                    ? [error(`heads[${i}]: accepts '${p.name}', which lifts Z, ` +
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
            ? [warn(`heads[${i}]: accepts nothing — no job can use this head`)]
            : [],
    );

/**
 * Duty limits are only meaningful as a complete set, which is the whole reason
 * they are one nested block. load.ts fills a missing number with 0 rather than
 * inventing one — these are measurements of a specific tool, not defaults — so
 * a partial block arrives here as a zero and is named precisely.
 *
 * minOnS >= maxOnS would leave the scheduler an empty band with no candidate
 * lift in it, forcing an inserted break every time.
 */
const dutyLimitsCoherent: Rule = (config) =>
    Object.entries(config.toolProfiles).flatMap(([name, t]) => {
        const d = t.dutyLimits;
        if (!d) return [];
        const p = `tools.${name}.dutyLimits`;
        const out: Issue[] = [];
        if (!(d.maxOnS > 0)) out.push(error(`${p}.maxOnS: must be > 0 (got ${d.maxOnS})`));
        if (!(d.minOnS > 0)) out.push(error(`${p}.minOnS: must be > 0 (got ${d.minOnS})`));
        if (!(d.dwellS > 0)) out.push(error(`${p}.dwellS: must be > 0 (got ${d.dwellS})`));
        if (d.settleS < 0) out.push(error(`${p}.settleS: must be >= 0 (got ${d.settleS})`));
        if (d.maxOnS > 0 && d.minOnS > 0 && d.minOnS >= d.maxOnS) {
            out.push(error(`${p}: minOnS (${d.minOnS}) must be < maxOnS (${d.maxOnS})`));
        }
        return out;
    });

/**
 * Homing recipes that would misbehave on the machine rather than in the parser.
 *
 * These are semantic checks, so they live here and not in load.ts: every one of
 * them parses fine and only goes wrong when a motor turns. The errors are the
 * ones with a physical consequence — a hard-stop crash, a datum in the wrong
 * place, or a leg that reports failure on healthy hardware.
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
            for (const k of ["pullInFeed", "sweepFeed", "budgetRevs",
                             "toleranceDeg"] as const) {
                if (h[k] <= 0) issues.push(error(`${p}.${k}: must be > 0`));
            }
            if (h.rampSteps < 0) issues.push(error(`${p}.rampSteps: must be >= 0`));
            if (h.pullInFeed > h.sweepFeed) {
                issues.push(error(`${p}.pullInFeed: must be <= sweepFeed (${h.sweepFeed})`));
            }
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
            // Not an error: a magnet glued 90° from the tool's zero is a real
            // machine. But a datum outside one revolution is almost always a
            // units mistake, and it lands silently in every coordinate after.
            if (Math.abs(h.datumDeg) >= 360) {
                issues.push(warn(
                    `${p}.datumDeg (${h.datumDeg}): outside one revolution — ` +
                    `this is an offset from the index, not an absolute angle`,
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

        // Every one of these is a divisor or a distance; a zero produces an
        // infinite step interval or a zero-step leg, neither of which the
        // firmware can act on sensibly.
        for (const k of ["hardTravel", "pullInFeed", "seekFeed", "latchFeed",
                         "backoffMm", "parkMm"] as const) {
            if (h[k] <= 0) issues.push(error(`${p}.${k}: must be > 0`));
        }
        if (h.rampSteps < 0) issues.push(error(`${p}.rampSteps: must be >= 0`));

        // The slow leg is what sets repeatability; if it is not slower than the
        // fast one, leg 3 is not a re-approach and the whole two-pass structure
        // buys nothing.
        if (h.latchFeed >= h.seekFeed) {
            issues.push(error(`${p}.latchFeed: must be < seekFeed (${h.seekFeed})`));
        }
        // A pull-in above the cruise makes the "ramp" a decel, so the axis hits
        // the switch at the FASTEST point of the leg.
        if (h.pullInFeed > h.seekFeed) {
            issues.push(error(`${p}.pullInFeed: must be <= seekFeed (${h.seekFeed})`));
        }
        // The seek runs at seekFeed for nearly its whole length, so the axis
        // ceiling applies to it exactly as it does to a job move.
        if (ax.maxFeed > 0 && h.seekFeed > ax.maxFeed) {
            issues.push(error(`${p}.seekFeed: exceeds ${path}.maxFeed (${ax.maxFeed})`));
        }
        // Leg 4 retracts parkMm from the switch, so a park at or beyond the far
        // end is not a position on this axis at all.
        if (h.parkMm >= h.hardTravel) {
            issues.push(error(`${p}.parkMm: must be < hardTravel (${h.hardTravel})`));
        }
        // Leg 3 re-approaches from the back-off point, so a back-off longer than
        // the park means the axis is left INSIDE the region leg 3 crossed —
        // legal, but it means leg 4 travelled less than leg 2 and the axis is
        // closer to the switch than it started. Usually a transposed pair.
        if (h.backoffMm > h.parkMm) {
            issues.push(warn(
                `${p}.backoffMm (${h.backoffMm}) > parkMm (${h.parkMm}) — ` +
                `the axis parks closer to the switch than it backed off`,
            ));
        }
        // Not an error: homing legitimately moves outside the soft envelope,
        // since no datum exists yet to measure that envelope from. But a
        // hardTravel under maxTravel means one of the two is simply wrong.
        if (ax.maxTravel > 0 && h.hardTravel < ax.maxTravel) {
            issues.push(warn(
                `${p}.hardTravel (${h.hardTravel}) < ${path}.maxTravel ` +
                `(${ax.maxTravel}) — the soft limit exceeds the physical frame`,
            ));
        }
        return issues;
    });

const RULES: readonly Rule[] = [
    homingCoherent,
    nonNegativeCeilings,
    nonNegativeTargets,
    xyMustBeCapped,
    targetsUnderCeilings,
    uniqueNodeIds,
    defaultHeadInRange,
    headsHaveAxesForAccepted,
    headsAcceptSomething,
    everyToolHasAHead,
    dutyLimitsCoherent,
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

function namedTargets(config: PipelineConfig): readonly [string, OpTarget][] {
    const { machine } = config;
    return [
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
}

/** min of two ceilings, ignoring 0 (uncapped). 0 if both uncapped. */
function minPositive(a: number, b: number): number {
    const capped = [a, b].filter((v) => v > 0);
    return capped.length ? Math.min(...capped) : 0;
}

/** The tool→machine chain, for reporting the value that will actually apply. */
function inherit(tool: OpTarget | undefined, machine: OpTarget): OpTarget {
    return { feed: tool?.feed ?? machine.feed, accel: tool?.accel ?? machine.accel };
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
