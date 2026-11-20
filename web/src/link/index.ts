/** urumi-host/link: a connection to the controller. */
export {
    stateName,
    faultNames,
    jobName,
    maskStr,
    STATE_NAMES,
    FAULT_NAMES,
    JOB_NAMES,
} from "./names.js";
export { Sink, LatestSink } from "./sink.js";
export { Writer, type WriterStats } from "./writer.js";

export {
    Demux,
    Ack,
    Nack,
    JogAck,
    CfgReply,
    makeSinks,
    type DemuxSinks,
    type DemuxStats,
} from "./demux.js";

export {
    Session,
    ListSource,
    JobSource,
    StreamContext,
    fatalReasonName,
    FATAL_STALL,
    FATAL_CRC_LIMIT,
    FATAL_JOB_LOST,
    type Hold,
    type PacketSource,
    type ReplayableSource,
    type SessionStats,
    type StreamResult,
    DEFAULT_WINDOW,
} from "./session.js";

export {
    Link,
    ConfigTransferError,
} from "./link.js";

// ── waiting for the machine (the host is acked ahead of the Pico) ──
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
} from "./settled.js";

// ── control-plane command helpers ──────────────────────────────────
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
} from "./commands.js";
