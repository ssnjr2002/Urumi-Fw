/**
 * link/settled.ts — poll the machine until it reaches a condition.
 *
 * Every UI built on this library grows this loop, because the host's view of
 * "done" runs ahead of the Pico's by the depth of its ring buffer: the last
 * packet is acked long before the last step is taken. Any command the firmware
 * gates on state (axis_map, setorigin, a fresh stream) must therefore wait for
 * the machine, not for the writer.
 *
 * Written five times across three demo files before it lived here, in four
 * subtly different shapes — one tolerated ALARM, three threw on it; one had a
 * timeout, three could hang forever; each carried its own UI update inline.
 * That last difference is why this takes an `onPoll` callback rather than
 * returning a stream: the caller wants to paint every sample, not just the
 * last one.
 *
 * This module deliberately knows nothing about a Link — only that something can
 * hand it a MachineStatus. That keeps it testable without a transport, and
 * usable against a recorded status log.
 */

import { MachineState, type MachineStatus } from "../format/status.js";
import { stateName } from "../format/names.js";
import type { AbortToken } from "./transport.js";

/** The slice of Link that settle() needs. */
export interface StatusSource {
    getStatus(timeoutMs?: number): Promise<MachineStatus>;
}

/** A condition on a status sample. Return true to stop polling. */
export type SettleCondition = (status: MachineStatus) => boolean;

export type SettleFailure = "timeout" | "fatal" | "aborted";

/**
 * Why the wait ended without the condition being met. Carries the last status
 * sample so a caller can report *what* the machine was doing instead.
 */
export class SettleError extends Error {
    constructor(
        readonly reason: SettleFailure,
        readonly status: MachineStatus | null,
        message: string,
    ) {
        super(message);
        this.name = "SettleError";
    }
}

export interface SettleOptions {
    /** Gap between polls, ms. Default 150 — the demos' figure. */
    pollMs?: number;
    /**
     * Give up after this long, ms. Default 0 = wait forever, which is what the
     * job-running paths want (a plan legitimately takes minutes).
     */
    timeoutMs?: number;
    /**
     * States that abandon the wait rather than satisfy it. Default ESTOP and
     * ALARM: if the machine has faulted it will never reach IDLE, so polling on
     * is just a slower way to time out. Pass `[]` to tolerate them — `atRest`
     * callers legitimately do, since a machine sitting in ALARM *is* at rest.
     */
    fatal?: readonly MachineState[];
    /** Called with every sample, including the one that ends the wait. */
    onPoll?: (status: MachineStatus) => void;
    /** Checked each iteration; set it to cancel a wait in progress. */
    abort?: AbortToken;
    /** Injectable clock + sleep, for tests. */
    now?: () => number;
    sleep?: (ms: number) => Promise<void>;
}

const DEFAULT_FATAL: readonly MachineState[] = [MachineState.ESTOP, MachineState.ALARM];

/**
 * Poll `src` until `until` holds, then return the sample that satisfied it.
 * Throws SettleError on timeout, on a fatal state, or if `abort` is set.
 *
 * The first poll happens immediately, so a machine already in the target state
 * costs one round trip and no delay.
 */
export async function settle(
    src: StatusSource,
    until: SettleCondition,
    options: SettleOptions = {},
): Promise<MachineStatus> {
    const {
        pollMs = 150,
        timeoutMs = 0,
        fatal = DEFAULT_FATAL,
        onPoll,
        abort,
        now = () => Date.now(),
        sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms)),
    } = options;

    const deadline = timeoutMs > 0 ? now() + timeoutMs : Infinity;
    let last: MachineStatus | null = null;

    for (;;) {
        if (abort?.isSet()) {
            throw new SettleError("aborted", last, "wait aborted");
        }

        last = await src.getStatus();
        onPoll?.(last);

        if (until(last)) return last;

        if (fatal.includes(last.state)) {
            throw new SettleError("fatal", last, `machine went ${stateName(last.state)}`);
        }

        // Checked after the poll, not before: a machine that reaches the target
        // exactly on the deadline has succeeded, and reporting a timeout there
        // would be a lie the caller cannot distinguish from a real one.
        if (now() >= deadline) {
            throw new SettleError(
                "timeout",
                last,
                `machine still ${stateName(last.state)} after ${timeoutMs} ms`,
            );
        }

        await sleep(pollMs);
    }
}

// ── conditions ───────────────────────────────────────────────────────────────

/**
 * Genuinely at rest: not RUNNING *and* the ring is empty. Both halves matter —
 * the firmware leaves RUNNING when it stops consuming, which is not the same
 * instant the buffer drains, and a command issued in that gap is rejected.
 *
 * Note `bufCount` is `undefined` on a getstate-parsed status (the text plane
 * cannot carry it), and undefined reads as empty here. That is correct for
 * settle(), which polls the binary STATUS_REQ, but means this condition is not
 * meaningful against a text-derived sample.
 */
export const atRest: SettleCondition = (s) => s.state !== MachineState.RUNNING && !s.bufCount;

/** In one specific state, buffer ignored. */
export function inState(target: MachineState): SettleCondition {
    return (s) => s.state === target;
}

/** In any of the given states. */
export function inAnyState(...targets: readonly MachineState[]): SettleCondition {
    return (s) => targets.includes(s.state);
}

/**
 * Convenience for the commonest wait, matching the shape callers had before:
 * resolves true if the machine came to rest, false if it did not, and never
 * throws for a fault. ALARM counts as rest — the machine is stationary, and the
 * caller's next move is to report or unalarm, not to keep waiting.
 */
export async function waitAtRest(
    src: StatusSource,
    timeoutMs = 5000,
    options: Omit<SettleOptions, "timeoutMs" | "fatal"> = {},
): Promise<boolean> {
    try {
        await settle(src, atRest, { ...options, timeoutMs, fatal: [], pollMs: options.pollMs ?? 50 });
        return true;
    } catch {
        return false;
    }
}

