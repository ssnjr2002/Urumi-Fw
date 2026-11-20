/** urumi-host/packets: the byte layouts on the wire. */
export { packBezier } from "./bezier.js";
export {
    packJobHdr, packLayerHdr, packToolHdr,
    type JobHdr, type LayerHdr, type ToolBox, type ToolHdr,
} from "./layer.js";


export { crc8, crc32 } from "./crc.js";

export { stampSeq } from "./packet.js";
export { packCjog, CJOG_SPEED_MAX, type JogDir } from "./cjog.js";

// ── wire format: STATUS_RSP + machine enums ──────────────────────────────────
export {
    MachineState,
    MachineContext,
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
    Selection,
    NO_LAYER,
    axisMask,
    AXIS_BITS,
    type AxisLetter,
} from "./status.js";
// ── wire format: remaining framing constants ──────────────────────────────────
export {
    MAGIC_ACK,
    MAGIC_NACK,
    MAGIC_ABORT,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
    MAGIC_STATUS_RSP,
    MAGIC_STATUS_RSP_V1,
    MAGIC_STATUS_RSP_V2,
    MAGIC_STATUS_RSP_V3,
    MAGIC_STATUS_RSP_V4,
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
    MAGIC_JOG_ACK,
    MAGIC_JOG_NACK,
    CJOG_SIZE,
    CJOG_SPEED_ONE,
    CJOG_DEADMAN_MS,
} from "./constants.js";

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
} from "./cfg.js";
