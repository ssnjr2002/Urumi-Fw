/** urumi-host/packets: the binary records on the wire (docs/reference/host-api/packets.md). */
export { packBezier, type BezierRecord, type Point } from "./bezier.js";
export {
    packJobHdr, packLayerHdr, packToolHdr,
    type JobHdr, type JobLayer, type LayerHdr, type ToolBox, type ToolHdr,
} from "./layer.js";
export { stampSeq } from "./packet.js";
export { packCjog, CJOG_SPEED_MAX, type JogDir } from "./cjog.js";
export {
    packCfgSetHeader,
    CFG_MAX_BYTES,
    CFG_SET_HDR_SIZE,
    CFG_DATA_HDR_SIZE,
    CfgNack,
} from "./cfg.js";
export {
    Nack,
    Refusal,
    parseNack,
    packNack,
    packJogNack,
    NACK_TEXT_MAX,
} from "./nack.js";
export { crc8, crc32 } from "./crc.js";
export * as names from "./names.js";
export {
    MachineStatus,
    parseStatusRsp,
    packStatusRsp,
    type PackStatusRspOptions,
    MachineState,
    MachineContext,
    JobReason,
    JoggingReason,
    HomingReason,
    ProbingReason,
    Fault,
    HomeFail,
    ProbeCause,
    Selection,
    NO_LAYER,
} from "./status.js";
export {
    MAGIC_BEZIER,
    BEZIER_SIZE,
    MAGIC_JOB_HDR,
    JOB_HDR_SIZE,
    MAGIC_TOOL_HDR,
    TOOL_HDR_SIZE,
    MAGIC_LAYER_HDR,
    LAYER_HDR_SIZE,
    MAGIC_CJOG,
    CJOG_SIZE,
    CJOG_DEADMAN_MS,
    HOST_TIMEOUT_MS,
    MAGIC_CJOG_STOP,
    MAGIC_JOG_ACK,
    MAGIC_JOG_NACK,
    MAGIC_STATUS_REQ,
    MAGIC_STATUS_RSP,
    STATUS_RSP_SIZE,
    MAGIC_ACK,
    MAGIC_NACK,
    MAGIC_SEQRESET,
    MAGIC_ABORT,
    MAGIC_CFG_SET,
    MAGIC_CFG_GET,
    MAGIC_CFG_RDY,
    MAGIC_CFG_ACK,
    MAGIC_CFG_NACK,
    MAGIC_CFG_DATA,
} from "./constants.js";
