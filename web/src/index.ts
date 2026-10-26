/**
 * Public API for the CNC toolpath library.
 *
 * SVG → annotated Béziers → wire packets, the machine config document, and
 * the comms layer (wire/link).
 *
 * This barrel is the *entire* supported surface. Anything not re-exported here
 * (geometry helpers, bezier math) is internal and may change without notice. The real-port
 * Transport backends (WebSerial, Node serialport) are NOT exported from this
 * barrel — import them from their own package subpaths so a Node consumer never
 * pulls browser globals:
 *
 *   import { WebSerialTransport } from "urumi-host/wire/link/backends/webserial";
 *   import { openNodeLink }       from "urumi-host/wire/link/backends/node";
 *
 * Those subpaths are in the `exports` map, and test/packageExports.test.ts
 * keeps them there.
 */

// ── machine: calibration + tool model ───────────────────────────────────────
// The model lives in ./machine/; ./machine/json/ is the document adapter, and
// loadConfig is the production entry. Hardcoded test machines are not part of
// the shipped tree at all — they live in test/machines.ts.
export {
    // loadConfig = parse ⨟ validate. Production callers want this one;
    // parseConfig proves shape only and is exposed for tests/tooling.
    loadConfig,
    type LoadResult,
    parseConfig,
    type ConfigResult,
} from "./machine/json/load.js";

export {
    validateConfig,
    type ValidationResult,
} from "./machine/json/validate.js";

export {
    CONFIG_BLOB_VERSION,
    encodeConfigBlob,
    decodeConfigBlob,
} from "./machine/json/blob.js";

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
    type HomingConfig,
    type LinearHoming,
    type RotaryHoming,
    type OpTarget,
    type BusNode,
    busNode,
    NodeType,
    type ResolvedAxes,
    resolvedAxesDefault,
    axesForHead,
    // heads + geometry references
    type ToolHead,
    toolHead,
    type ReferencePoint,
    type ToolOffset,
    type LaserPointer,
    // tools
    ToolType,
    type ToolProfile,
    toolProfile,
    type DutyLimits,
    type ToolPower,
    // tool presets + lookup tables (demo uses TOOL_PROFILES_BY_TYPE)
    PEN,
    KNIFE,
    CREASE,
    REVOLVER_PEN,
    TOOL_PROFILES,
    TOOL_PROFILES_BY_TYPE,
} from "./machine/index.js";

export {
    toolForLayer,
    requiredAxes,
    headsAccepting,
    canRunTool,
    resolveTargets,
    type ResolvedTargets,
} from "./machine/resolve.js";


export { DEFAULTS } from "./machine/defaults.js";


// geometry types that appear in the public signatures above
export {
    type Pt,
    type CubicBezier,
} from "./toolpath/geometry.js";

// ── Bézier path: SVG layer → annotated Béziers → BEZIER packets ─────────────
export { loadSvgPaths, type Subpath, type LoadedSvg, type LoadOptions } from "./svg/load.js";
export { cleanSubpath, DEFAULT_CLEAN_OPTIONS, type CleanSubpath, type CleanOptions, type JoinKind } from "./toolpath/clean.js";
export {
    annotate,
    analyzeBezier,
    BezierFlag,
    DEFAULT_ANNOTATE_OPTIONS,
    type AnnotatedBezier,
    type AnnotateOptions,
} from "./toolpath/annotate.js";
export { prepareBezierJob, type BezierJob, type BezierJobOptions, type BBox } from "./toolpath/job.js";
export { packBezier } from "./wire/format/bezier.js";
export { packLayerHdr, type LayerHdr } from "./wire/format/layer.js";


export { crc8, crc32 } from "./wire/format/crc.js";

export { stampSeq } from "./wire/format/packet.js";
export { packCjog, CJOG_SPEED_MAX, type JogDir } from "./wire/format/cjog.js";

// ── wire format: STATUS_RSP + machine enums ──────────────────────────────────
export {
    MachineState,
    Fault,
    HomeFail,
    Probing,
    ProbeCause,
    JobReason,
    MachineStatus,
    parseGetReply,
    getValue,
    statusFromGet,
    GetMarker,
    parseStatusRsp,
    packStatusRsp,
    axisMask,
    AXIS_BITS,
    type AxisLetter,
} from "./wire/format/status.js";

export {
    stateName,
    faultNames,
    jobName,
    maskStr,
    STATE_NAMES,
    FAULT_NAMES,
    JOB_NAMES,
} from "./wire/format/names.js";

// ── wire format: remaining framing constants ──────────────────────────────────
export {
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
    NACK_SOFT_LIMIT,
    NACK_MIXED_AXES,
    NACK_BUSY,
    NACK_JOB,
    NACK_TOOL_UNKNOWN,
    NACK_TOOL_CHANGE,
    MAGIC_LAYER_HDR,
    LAYER_HDR_SIZE,
    MAGIC_CJOG,
    MAGIC_CJOG_STOP,
    CJOG_SIZE,
    CJOG_SPEED_ONE,
    CJOG_DEADMAN_MS,
} from "./wire/format/constants.js";

export {
    CFG_DATA_HDR_SIZE,
    MAX_CFG_PAYLOAD,
    CFG_NACK_CRC,
    CFG_NACK_TOO_BIG,
    CFG_NACK_BAD_STATE,
    CFG_NACK_FLASH,
    CFG_NACK_TIMEOUT,
    CFG_NACK_SCHEMA,
    CFG_SET_HDR_SIZE,
    CFG_MAX_BYTES,
    packCfgSetHeader,
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
    ConfigTransferError,
    type Attachable,
} from "./wire/link/link.js";

// ── wire/link: waiting for the machine (the host is acked ahead of the Pico) ──
export {
    settle,
    waitAtRest,
    atRest,
    inState,
    inAnyState,
    SettleError,
    type StatusSource,
    type SettleCondition,
    type SettleOptions,
    type SettleFailure,
} from "./wire/link/settled.js";

// ── wire/link: control-plane command helpers ──────────────────────────────────
export {
    ping,
    pingNode,
    pingAll,
    get,
    getKeys,
    GET_MAX_KEYS,
    STATE_KEYS,
    getState,
    getStatus,
    getPos,
    nodePos,
    nodeStat,
    vacServo,
    vacPump,
    vacSwitch,
    knifeOsc,
    knifeBlower,
    enable,
    disable,
    setOrigin,
    linLeg,
    rotLeg,
    parkLeg,
    legAbort,
    probeMap,
    probeLeg,
    probeEnd,
    setProbe,
    unprobe,
    type ProbeLegArgs,
    type ProbeReply,
    pause,
    cancel,
    stop,
    unalarm,
    axisMap,
    readAxisMap,
    type SlotBinding,
    jog,
    type JogAxis,
} from "./wire/link/commands.js";

// ── wire/link backends: Sim (env-agnostic; real ports import from the subpath) ─
export { SimTransport } from "./wire/link/backends/sim.js";

