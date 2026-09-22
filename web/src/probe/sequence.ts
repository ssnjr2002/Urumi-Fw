/**
 * probe/sequence.ts — run a probe plan against a Link and store the result.
 *
 * `probe_map`, four arm-and-wait legs, `probe_end`, `setprobe`. The contact
 * height is `getpos` Z read after the latch leg. A failed leg tears the session
 * down on the Pico (docs/tool_probe.md §5.11.2); any other failure here closes
 * it with `probe_end` before rethrowing.
 */

import type { Link } from "../wire/link/link.js";
import {
    getPos, getState, probeEnd, probeLeg, probeMap, setProbe,
} from "../wire/link/commands.js";
import { MachineState, Probing, ProbeCause, type MachineStatus } from "../wire/format/status.js";
import type { ProbePlan, ProbeStep } from "./derive.js";

export interface RunProbeOptions {
    /** Gap between status polls, ms. */
    pollMs?: number;
    /** Per-leg ceiling, ms; a backstop for a link that stopped answering. */
    legTimeoutMs?: number;
    onLeg?: (leg: ProbeStep, index: number, total: number) => void;
}

export class ProbeError extends Error {
    constructor(
        readonly leg: ProbeStep | null,
        readonly status: MachineStatus | null,
        message: string,
    ) {
        super(message);
        this.name = "ProbeError";
    }
}

const CAUSE_TEXT: Record<number, string> = {
    [ProbeCause.BUDGET]: "never reached the switch within the leg's budget",
    [ProbeCause.POLL]: "the switch node stopped answering",
    [ProbeCause.CHATTER]: "the switch chattered past the retry limit",
    [ProbeCause.ALREADY_OPEN]: "the switch was already open when the leg armed",
    [ProbeCause.NOT_CLEARED]: "the retract finished but the switch is still open",
    [ProbeCause.POS_MISMATCH]: "the Z node's counter disagrees with the Pico — datum lost",
    [ProbeCause.DEADLINE]: "supervisor timeout — datum lost",
    [ProbeCause.ESTOP]: "stopped by an e-stop — datum lost",
};

const sleep = (ms: number): Promise<void> => new Promise((r) => setTimeout(r, ms));

async function awaitLeg(
    link: Link,
    leg: ProbeStep,
    opts: RunProbeOptions,
): Promise<MachineStatus> {
    const pollMs = opts.pollMs ?? 100;
    const timeoutMs = opts.legTimeoutMs ?? 60_000;
    const t0 = Date.now();
    for (;;) {
        const st = await getState(link);
        if (st.state !== MachineState.PROBING) {
            const cause = st.probeCause;
            throw new ProbeError(leg, st,
                `probe ${leg.name}: ${cause !== undefined ? CAUSE_TEXT[cause] ?? `cause ${cause}` : "session ended"}`);
        }
        if (st.probing !== Probing.LEG) {
            const want = leg.args.retract ? Probing.CLEAR : Probing.CONTACT;
            if (st.probing !== want) {
                throw new ProbeError(leg, st,
                    `probe ${leg.name}: ended ${st.probing === Probing.CONTACT ? "on" : "off"} ` +
                        `the switch, expected ${want === Probing.CONTACT ? "on" : "off"}`);
            }
            return st;
        }
        if (timeoutMs > 0 && Date.now() - t0 > timeoutMs) {
            throw new ProbeError(leg, st, `probe ${leg.name}: no result after ${timeoutMs} ms`);
        }
        await sleep(pollMs);
    }
}

/**
 * Probe the tool in the engaged head and store the result on the Pico.
 * Z must be above the switch and clear of it. Returns the contact height,
 * wire steps.
 */
export async function runProbe(
    link: Link,
    plan: ProbePlan,
    opts: RunProbeOptions = {},
): Promise<number> {
    const opened = await probeMap(link, plan.switchNode);
    if (!opened.ok) throw new ProbeError(null, null, `probe_map refused (${opened.reason})`);

    let contact: number | null = null;
    try {
        for (let i = 0; i < plan.legs.length; i++) {
            const leg = plan.legs[i]!;
            opts.onLeg?.(leg, i, plan.legs.length);
            const armed = await probeLeg(link, leg.args);
            if (!armed.ok) {
                throw new ProbeError(leg, null, `probe ${leg.name}: refused to arm (${armed.reason})`);
            }
            await awaitLeg(link, leg, opts);
            if (leg.name === "latch") contact = (await getPos(link))[2];
        }
    } catch (e) {
        // A failed leg has already closed the session; this is for the rest.
        const st = await getState(link).catch(() => null);
        if (st?.state === MachineState.PROBING) await probeEnd(link).catch(() => undefined);
        throw e;
    }

    const closed = await probeEnd(link);
    if (!closed.ok) throw new ProbeError(null, null, `probe_end refused (${closed.reason})`);
    if (contact === null) throw new ProbeError(null, null, "probe plan has no latch leg");

    const stored = await setProbe(link, contact);
    if (!stored.ok) {
        throw new ProbeError(null, null, `setprobe ${contact} refused (${stored.reason})`);
    }
    return contact;
}
