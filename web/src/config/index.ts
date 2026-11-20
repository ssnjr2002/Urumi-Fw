/**
 * config/index.ts (urumi-host/config) — the machine model and its document.
 *
 *   schema.ts     types and factories
 *   defaults.ts   default values
 *   tools.ts      tool presets
 *   resolve.ts    resolution policy (which axes, which feed, can it run)
 *
 * load, validate, blob, save, image and mesh are the only files that know a
 * config document exists; they import the model, never the reverse. Test
 * machines live in test/machines.ts, outside the shipped tree.
 */

export * from "./schema.js";
export * from "./tools.js";
export * from "./resolve.js";
export { DEFAULTS } from "./defaults.js";
export { parseConfig, loadConfig, type ConfigResult, type LoadResult } from "./load.js";
export { validateConfig, type ValidationResult } from "./validate.js";
export { modelToFile } from "./save.js";
export { CONFIG_BLOB_VERSION, encodeConfigBlob, decodeConfigBlob } from "./blob.js";
// Config files are JSONC (config/controller.jsonc); strip before loadConfig.
export { stripJsonComments } from "./image.js";
