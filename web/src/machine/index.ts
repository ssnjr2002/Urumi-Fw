/**
 * machine/index.ts — barrel over the machine model.
 *
 *   schema.ts     types and factories
 *   defaults.ts   default values
 *   tools.ts      tool presets
 *   resolve.ts    resolution policy (which axes, which feed, can it run)
 *
 * `json/` is the only part that knows a config document exists (load,
 * validate, blob, save); it imports the model, never the reverse. Test
 * machines live in test/machines.ts, outside the shipped tree.
 */

export * from "./schema.js";
export * from "./tools.js";
export * from "./resolve.js";
export { DEFAULTS } from "./defaults.js";
export { parseConfig, loadConfig, type ConfigResult, type LoadResult } from "./json/load.js";
export { validateConfig, type ValidationResult } from "./json/validate.js";
export { modelToFile } from "./json/save.js";
