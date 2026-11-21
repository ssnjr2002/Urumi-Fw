/**
 * config/index.ts (urumi-host/config): the controller's config in its three
 * forms, JSON, config and encoded config (docs/reference/host-api/config.md).
 *
 *   schema.ts     the config's types
 *   defaults.ts   default values
 *   tools.ts      tool presets
 *   load, validate, save, blob   JSON → config → encoded config, and back
 *
 * Private: the factories, resolve.ts (which axes, which feed, can it run),
 * the /config.bin and /mesh.bin builders (image.ts, mesh.ts, for
 * web/scripts/) and the runtime table. Tests import those from their files.
 */

export {
    NodeType,
    ToolType,
    type AxisConfig,
    type BedPoint,
    type BusNode,
    type ControllerConfig,
    type DutyLimits,
    type HomingConfig,
    type JobBuffer,
    type JobFeed,
    type LaserPointer,
    type LinearHoming,
    type MachineConfig,
    type OpTarget,
    type ProbeConfig,
    type ProbeSwitch,
    type QualityConfig,
    type ReferencePoint,
    type RotaryHoming,
    type StoredPositions,
    type ToolHead,
    type ToolOffset,
    type ToolPower,
    type ToolProfile,
    type WorkArea,
} from "./schema.js";
export { PEN, KNIFE, CREASE, REVOLVER_PEN, TOOL_PROFILES } from "./tools.js";
export { DEFAULTS } from "./defaults.js";
export { loadConfig, type LoadResult } from "./load.js";
export { validateConfig, type ValidationResult } from "./validate.js";
export { saveConfig } from "./save.js";
export { encodeConfig, decodeConfig } from "./blob.js";
// Until loadConfig reads JSONC itself.
export { stripJsonComments } from "./image.js";
