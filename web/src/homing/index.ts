/**
 * homing/ — a home, from config to datum (docs/homing.md §3).
 *
 * A sibling of operatorJog/ for the same reason it is: config-driven motion
 * that is not a job. It is not in machine/ (that is the model, not motion), not
 * in controller/ (that runs jobs), and not folded into operatorJog/ (unbounded
 * manual motion, versus a closed sequence with a datum as its outcome).
 *
 * The split inside is the load-bearing part:
 *
 *   derive.ts    PURE. Config → the legs, and for a linear axis the datum too.
 *                Every number that could
 *                crash an axis is computed here, testable with nothing plugged
 *                in.
 *   sequence.ts  The part that needs a machine: arm, poll, judge the verdict.
 *
 * The wire verbs it drives (`lin_leg`, `rot_leg`, `setorigin`) stay in wire/link/
 * commands.ts, and the schema stays in machine/schema.ts — this module owns the
 * arithmetic and the sequencing, not the transport or the config shape.
 */

export {
    derivePlan, deriveRotaryPlan, approachDir,
    resolveRotaryIndex, foldSigned, type ResolvedIndex,
} from "./derive.js";
export {
    runHoming, runRotaryHoming, HomingError,
    type RunHomingOptions, type SweepResult, type RotaryHomingResult,
} from "./sequence.js";
export {
    LegKind, type HomingLeg, type HomingPlan, type RotaryHomingPlan,
} from "./types.js";
