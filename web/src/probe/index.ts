/**
 * probe/ — measure a tool's contact height on the bed switch
 * (docs/tool_probe.md, docs/tool_probe_planner_integration.md).
 *
 *   derive.ts    PURE. ProbeConfig → the four legs.
 *   sequence.ts  Arm, poll, read the contact, store it with `setprobe`.
 */

export { deriveProbePlan, type ProbePlan, type ProbeStep } from "./derive.js";
export { runProbe, ProbeError, type RunProbeOptions } from "./sequence.js";
