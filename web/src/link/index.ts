/** urumi-host/link: a connection to the controller. */
export { stateName, faultNames, jobName, nackName, cfgNackName } from "./names.js";

export {
    Session,
    JobSource,
    type Hold,
    type Fatal,
    type SessionStats,
    type SessionResult,
} from "./session.js";

export {
    Link,
    CJog,
    ConfigTransferError,
    type CJogDirection,
    type CJogReply,
    type LinkOptions,
    type LinkStats,
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
    unalarm,
    axisMap,
    readAxisMap,
    type SlotBinding,
    jog,
    type JogAxis,
} from "./commands.js";
