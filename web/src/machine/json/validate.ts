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

/** A seeded tool that steers A on a head with no A node can never run. */
const headsSupportSeededTools: Rule = ({ machine }) =>
    machine.heads.flatMap((h, i) =>
        h.profile &&
        (h.profile.tangential || h.profile.slotOffsets !== undefined) &&
        !h.a.node.present
            ? [
                  error(
                      `heads[${i}]: seeded tool '${h.profile.name}' steers A, ` +
                          "but that head's A node is not present",
                  ),
              ]
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

const RULES: readonly Rule[] = [
    nonNegativeCeilings,
    nonNegativeTargets,
    xyMustBeCapped,
    targetsUnderCeilings,
    uniqueNodeIds,
    defaultHeadInRange,
    headsSupportSeededTools,
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
