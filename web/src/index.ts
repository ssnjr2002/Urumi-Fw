/**
 * Public API for the CNC toolpath library.
 *
 * SVG + machine config → .plan → schedule → wire packets.
 *
 * This barrel is the *entire* supported surface. Anything not re-exported here
 * (individual pipeline stages like flatten/constrain/plan, geometry helpers,
 * bezier math) is internal and may change without notice. The browser
 * `SerialTransport` lives in a separate entry point and is intentionally not
 * exported here so Node consumers don't pull in WebSerial.
 */

// ── config: machine calibration + tool model ────────────────────────────────
// JSON → PipelineConfig is the production entry. defaultConfig()/uniformMachine()
// are internal test fixtures — not exported; tests import them directly from
// ./config/config.js.
export {
    parseConfig,
    type ConfigResult,
} from "./config/configLoader.js";

export {
    validateConfig,
    type ValidationResult,
} from "./config/validateConfig.js";

export {
    // top-level config
    type PipelineConfig,
    pipelineConfig,
    type MachineConfig,
    machineConfig,
    type QualityConfig,
    qualityConfig,
    // axes / bus
    type AxisConfig,
    axisConfig,
    type OpTarget,
    type BusNode,
    busNode,
    NodeType,
    type ResolvedAxes,
    resolvedAxes,
    // heads + geometry references
    type ToolHead,
    toolHead,
    type ReferencePoint,
    type ToolOffset,
    type LaserPointer,
    OFFSET_TOLERANCE_MM,
    // tools
    ToolType,
    type ToolProfile,
    toolProfile,
    needsOffsetComp,
    // tool presets + lookup tables (demo uses TOOL_PROFILES_BY_TYPE)
    PEN,
    KNIFE,
    CREASE,
    REVOLVER_PEN,
    TOOL_PROFILES,
    TOOL_PROFILES_BY_TYPE,
} from "./config/config.js";

export {
    toolForLayer,
    requiredAxes,
    canRunTool,
} from "./config/helpers.js";

// ── svg ingestion (SVG text → curves in mm) ─────────────────────────────────
// bakePlan handles ingestion internally; these are exposed for callers who want
// to preview/inspect geometry (e.g. render an SVG before baking).
export {
    loadSvg,
    loadSvgSubpaths,
    loadSvgLayers,
    loadSvgMm,
    loadSvgMmSubpaths,
    loadSvgMmLayers,
    parseViewport,
    makeTransform,
    applyTransform,
    type Viewport,
    // parser injection: default is the global DOMParser; Node consumers inject one
    setDOMParser,
    type DOMParserLike,
} from "./svg/ingest.js";

// geometry types that appear in the public signatures above
export {
    type Pt,
    type CubicBezier,
} from "./toolpath/geometry.js";

// ── production bake: SVG + config → Plan + .plan bytes ───────────────────────
// The primary entry point most consumers want (demo/main.js).
export {
    bakePlan,
    type BakePlanOptions,
    assembleBlocks,
} from "./production/bakePlan.js";

export {
    compileBlock,
    type CompileBlockResult,
} from "./production/compileBlock.js";

// ── plan model + .plan file codec ───────────────────────────────────────────
export {
    type Plan,
    type Block,
    planToolTypes,
    feasibleOn,
} from "./plan/plan.js";

export {
    savePlan,
    loadPlan,
    PLAN_MAGIC,
    PLAN_VERSION,
    SLOT_NONE,
} from "./plan/planFile.js";

// ── orchestrate: mount scheduling + runtime walk ────────────────────────────
export {
    scheduleMounts,
    mountDiff,
    type Schedule,
    type Phase,
    type MountSet,
} from "./orchestrate/schedule.js";

export {
    walkSchedule,
    type WalkEvent,
    type WalkState,
    type WalkOptions,
} from "./orchestrate/walk.js";

// ── wire format: MicroSegment + packet packer ───────────────────────────────
export {
    type MicroSegment,
    microSegment,
    interval,
    MICRO_PATH_END,
    MICRO_PAUSE,
    MICRO_LIFT,
    MICRO_JOG,
} from "./wire/microsegment.js";

export {
    packMicrosegment,
    serialiseMicrosegments,
    writeStream,
    decodePacket,
    crc8,
    type DecodedPacket,
    MAGIC_MICROSEG,
    PACKET_SIZE,
    FRAME_PREFIX_SIZE,
    FRAMED_PACKET_SIZE,
} from "./wire/packet.js";
