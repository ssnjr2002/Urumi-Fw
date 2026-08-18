/**
 * machine/index.ts — barrel over the machine model. Import site for src/.
 *
 * This directory answers "what is true of this machine". It knows nothing
 * about JSON, files, or a Link, and imports nothing outside itself:
 *
 *   schema.ts     TYPES + factories, nothing else
 *   defaults.ts   default VALUES — the only place a policy number lives
 *   tools.ts      tool preset catalogue (data; changes when you build a tool)
 *   resolve.ts    resolution policy (which axes, which feed, can it run)
 *   frames.ts     home↔tool coordinate transforms (docs/coordinate_frames_and_limits.md)
 *
 * The `json/` subdirectory is the ADAPTER, and only it knows a config document
 * exists:
 *
 *   json/load.ts      JSON → PipelineConfig, structure only
 *   json/validate.ts  semantic rules, one array; loadConfig() runs it after parse
 *
 * The direction is one-way — json/ imports the model, never the reverse — so
 * the model stays usable by anything holding a MachineConfig, however it was
 * obtained. That is the whole reason the split exists: "config" used to mean
 * both the document and everything derived from it, and files like frames.ts
 * are derived geometry, not a file format.
 *
 * Hardcoded test machines are NOT here: they live in test/machines.ts, outside
 * the shipped tree. Importing a hardcoded 160 steps/mm machine into a
 * production path is exactly the accident the split exists to prevent.
 */

export * from "./schema.js";
export * from "./tools.js";
export * from "./resolve.js";
export * from "./frames.js";
export { DEFAULTS } from "./defaults.js";
export { parseConfig, loadConfig, type ConfigResult, type LoadResult } from "./json/load.js";
export { validateConfig, type ValidationResult } from "./json/validate.js";
