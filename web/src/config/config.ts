/**
 * config.ts — barrel over the config module. Import site for the rest of src/.
 *
 * The config layer is seven files, each with one job:
 *
 *   defaults.ts   default VALUES — the only place a policy number lives
 *   schema.ts     TYPES + factories, nothing else
 *   tools.ts      tool preset catalogue (data; changes when you build a tool)
 *   load.ts       JSON → PipelineConfig, structure only
 *   validate.ts   semantic rules, one array; loadConfig() runs it after parse
 *   resolve.ts    resolution policy (which axes, which feed, can it run)
 *   frames.ts     home↔tool coordinate transforms (docs/coordinate_frames_and_limits.md)
 *   fixtures.ts   hardcoded test machines — NOT re-exported here on purpose
 *
 * fixtures.ts is deliberately absent from this barrel: importing a hardcoded
 * 160 steps/mm machine into a production path is exactly the accident the
 * split exists to prevent. Tests import it directly, by its honest name.
 */

export * from "./schema.js";
export * from "./tools.js";
export * from "./resolve.js";
export * from "./frames.js";
export { DEFAULTS } from "./defaults.js";
export { parseConfig, loadConfig, type ConfigResult, type LoadResult } from "./load.js";
export { validateConfig, type ValidationResult } from "./validate.js";
