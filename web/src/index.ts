/**
 * Public API for the CNC toolpath library.
 *
 * SVG + machine config → .plan → schedule → wire packets.
 *
 * Plus the comms layer (wire/link) — the real-time RS485 transport
 * abstraction, and operatorJog for manual tap/click jogging with blend.
 *
 * This barrel is the *entire* supported surface. Anything not re-exported here
 * (individual pipeline stages like flatten/constrain/plan, geometry helpers,
 * bezier math) is internal and may change without notice. The real-port
 * Transport backends (WebSerial, Node serialport) are NOT exported from this
 * barrel — import them directly from wire/link/backends/ so a Node consumer
 * never pulls browser globals.
 */

// ── config: machine calibration + tool model ────────────────────────────────
// JSON → PipelineConfig is the production entry. defaultConfig()/uniformMachine()
// are internal test fixtures — not exported; tests import them directly from
// ./config/config.js.
export {
    // loadConfig = parse ⨟ validate. Production callers want this one;
    // parseConfig proves shape only and is exposed for tests/tooling.
    loadConfig,
    type LoadResult,
    parseConfig,
    type ConfigResult,
} from "./config/load.js";

export {
    validateConfig,
    type ValidationResult,
} from "./config/validate.js";

export {
    // top-level config
    type PipelineConfig,
    type MachineConfig,
    machineConfig,
    type QualityConfig,
    qualityConfig,
    // axes / bus
    type AxisConfig,
    axisConfig,
    type OpTarget,
    type MachineTarget,
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
    type DutyLimits,
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
    resolveTargets,
    type ResolvedTargets,
} from "./config/resolve.js";

export { DEFAULTS } from "./config/defaults.js";

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
    MICRO_DUTY_RELEASE,
    MICRO_DUTY_ASSERT,
} from "./wire/format/microsegment.js";

export {
    scheduleDutyBreaks,
    segmentSeconds,
    type DutyBreakResult,
} from "./production/dutyBreaks.js";

export { crc8 } from "./wire/format/crc.js";
export { MAGIC_MICROSEG, PACKET_SIZE } from "./wire/format/constants.js";

export {
    packMicrosegment,
    serialiseMicrosegments,
    writeStream,
    decodePacket,
    type DecodedPacket,
    FRAME_PREFIX_SIZE,
    FRAMED_PACKET_SIZE,
} from "./wire/format/packet.js";

export { packJog, stampSeq, unpackMicrosegment } from "./wire/format/packet.js";

// ── wire format: STATUS_RSP + machine enums ──────────────────────────────────
export {
    MachineState,
    AlarmReason,
    RunningReason,
    MachineStatus,
    parseGetstate,
    parseStatusRsp,
    packStatusRsp,
    axisMask,
    AXIS_BITS,
    type AxisLetter,
} from "./wire/format/status.js";

// ── wire format: remaining framing constants ──────────────────────────────────
export {
    MAGIC_JOG,
    MAGIC_ACK,
    MAGIC_NACK,
    MAGIC_ABORT,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
    MAGIC_STATUS_RSP,
    MAGIC_STATUS_RSP_V1,
    MAGIC_CFG_SET,
    MAGIC_CFG_GET,
    MAGIC_CFG_RDY,
    MAGIC_CFG_ACK,
    MAGIC_CFG_NACK,
    MAGIC_CFG_DATA,
    STATUS_RSP_SIZE,
    NACK_CRC,
    NACK_FULL,
    NACK_BAD_MAGIC,
    NACK_PAUSED,
    NACK_BAD_STATE,
    NACK_ABORTING,
} from "./wire/format/constants.js";

export {
    CFG_DATA_HDR_SIZE,
    MAX_CFG_PAYLOAD,
    CFG_NACK_CRC,
    CFG_NACK_TOO_BIG,
    CFG_NACK_BAD_STATE,
    CFG_NACK_FLASH,
    CFG_NACK_TIMEOUT,
    packCfgDataHeader,
    unpackCfgDataHeader,
    type CfgDataHeader,
} from "./wire/format/cfg.js";

// ── wire/link: transport abstraction ──────────────────────────────────────────
export {
    type Transport,
    type Writable,
    type Readable,
    type AbortToken,
    AbortFlag,
} from "./wire/link/transport.js";

export { Sink, LatestSink } from "./wire/link/sink.js";
export { Writer, type WriterStats } from "./wire/link/writer.js";

export {
    Demux,
    Ack,
    Nack,
    CfgReply,
    makeSinks,
    type DemuxSinks,
    type DemuxStats,
} from "./wire/link/demux.js";

export {
    Session,
    ListSource,
    StreamContext,
    fatalReasonName,
    FATAL_STALL,
    FATAL_CRC_LIMIT,
    type PacketSource,
    type SessionStats,
    type StreamResult,
    DEFAULT_WINDOW,
} from "./wire/link/session.js";

export {
    Link,
    type Attachable,
} from "./wire/link/link.js";

// ── wire/link: control-plane command helpers ──────────────────────────────────
export {
    ping,
    pingNode,
    pingAll,
    getState,
    getStatus,
    getPos,
    nodePos,
    vacServo,
    vacPump,
    vacSwitch,
    knifeOsc,
    knifeBlower,
    enable,
    disable,
    setOrigin,
    pause,
    resume,
    cancel,
    stop,
    unalarm,
    axisMap,
    readAxisMap,
    type SlotBinding,
} from "./wire/link/commands.js";

// ── wire/link backends: Sim (env-agnostic; real ports import from the subpath) ─
export { SimTransport } from "./wire/link/backends/sim.js";

// ── operatorJog ──────────────────────────────────────────────────────────────
export {
    makeJog,
    ClickJogSource,
    jogClick,
    jogTo,
    jogToPoint,
    type JogTarget,
    type JogHandle,
    type ClickJogSourceOptions,
    type JogToOptions,
    type AxisCalibration,
} from "./operatorJog/index.js";
