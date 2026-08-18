/**
 * settled.test.ts — the poll loop, driven off a scripted status sequence.
 *
 * No Link, no transport: settle() takes a StatusSource, so a plain array of
 * samples is a complete test fixture. Time is injected too, so a 5 s timeout
 * costs no wall clock.
 */

import { describe, it, expect } from "vitest";
import {
    settle,
    waitAtRest,
    atRest,
    inState,
    inAnyState,
    SettleError,
    type StatusSource,
} from "../../../src/wire/link/settled.js";
import {
    MachineState,
    MachineStatus,
    AlarmReason,
    RunningReason,
} from "../../../src/wire/format/status.js";
import { AbortFlag } from "../../../src/wire/link/transport.js";

/** A status sample; only state and bufCount matter to these conditions. */
function st(state: MachineState, bufCount = 0): MachineStatus {
    return new MachineStatus(state, 0, 0, AlarmReason.NONE, RunningReason.JOB, bufCount);
}

/** Replays `samples` in order, repeating the last one forever. */
function source(samples: readonly MachineStatus[]): StatusSource & { polls: number } {
    let i = 0;
    return {
        polls: 0,
        async getStatus() {
            this.polls++;
            return samples[Math.min(i++, samples.length - 1)]!;
        },
    };
}

/** Injected clock that advances by the sleep amount — no real waiting. */
function clock() {
    let t = 0;
    return {
        now: () => t,
        sleep: (ms: number) => {
            t += ms;
            return Promise.resolve();
        },
    };
}

describe("settle", () => {
    it("returns the first sample when the condition already holds", async () => {
        const src = source([st(MachineState.IDLE)]);
        const got = await settle(src, inState(MachineState.IDLE), clock());
        expect(got.state).toBe(MachineState.IDLE);
        expect(src.polls).toBe(1); // no sleep before the first poll
    });

    it("polls until the condition holds and returns that sample", async () => {
        const src = source([
            st(MachineState.RUNNING, 9),
            st(MachineState.RUNNING, 3),
            st(MachineState.IDLE, 0),
        ]);
        const got = await settle(src, atRest, clock());
        expect(got.bufCount).toBe(0);
        expect(src.polls).toBe(3);
    });

    it("does not treat a drained buffer in RUNNING as at rest, nor IDLE with a full one", async () => {
        // The two halves of atRest, each on its own. This is the gap that made
        // the demos need it: the host is acked ahead of the machine.
        expect(atRest(st(MachineState.RUNNING, 0))).toBe(false);
        expect(atRest(st(MachineState.IDLE, 4))).toBe(false);
        expect(atRest(st(MachineState.IDLE, 0))).toBe(true);
    });

    it("throws on a fatal state rather than polling on forever", async () => {
        const src = source([st(MachineState.RUNNING, 1), st(MachineState.ALARM)]);
        const err = await settle(src, inState(MachineState.IDLE), clock()).catch((e) => e);
        expect(err).toBeInstanceOf(SettleError);
        expect(err.reason).toBe("fatal");
        expect(err.status.state).toBe(MachineState.ALARM);
        expect(err.message).toContain("ALARM"); // named, not a bare number
    });

    it("honours an empty fatal list, so a caller can tolerate ALARM", async () => {
        const src = source([st(MachineState.ALARM, 0)]);
        const got = await settle(src, atRest, { ...clock(), fatal: [] });
        expect(got.state).toBe(MachineState.ALARM);
    });

    it("times out with the last state in the message", async () => {
        const src = source([st(MachineState.RUNNING, 5)]);
        const err = await settle(src, atRest, { ...clock(), timeoutMs: 500 }).catch((e) => e);
        expect(err.reason).toBe("timeout");
        expect(err.message).toContain("RUNNING");
        expect(err.message).toContain("500");
    });

    it("waits forever by default", async () => {
        // 200 RUNNING samples then IDLE: no timeout means it keeps going.
        const src = source([...Array(200).fill(st(MachineState.RUNNING, 1)), st(MachineState.IDLE)]);
        const got = await settle(src, atRest, clock());
        expect(got.state).toBe(MachineState.IDLE);
        expect(src.polls).toBe(201);
    });

    it("succeeds on the deadline rather than reporting a false timeout", async () => {
        // Condition met on the sample taken exactly at t = timeoutMs.
        const src = source([st(MachineState.RUNNING, 1), st(MachineState.IDLE)]);
        const got = await settle(src, atRest, { ...clock(), timeoutMs: 150, pollMs: 150 });
        expect(got.state).toBe(MachineState.IDLE);
    });

    it("reports every sample to onPoll, including the last", async () => {
        const src = source([st(MachineState.RUNNING, 2), st(MachineState.IDLE)]);
        const seen: MachineState[] = [];
        await settle(src, atRest, { ...clock(), onPoll: (s) => seen.push(s.state) });
        expect(seen).toEqual([MachineState.RUNNING, MachineState.IDLE]);
    });

    it("stops when the abort token is set", async () => {
        const abort = new AbortFlag();
        const src = source([st(MachineState.RUNNING, 1)]);
        const spy: StatusSource = {
            async getStatus() {
                const s = await src.getStatus();
                abort.set(); // cancelled while we were waiting on the wire
                return s;
            },
        };
        const err = await settle(spy, atRest, { ...clock(), abort }).catch((e) => e);
        expect(err.reason).toBe("aborted");
        expect(src.polls).toBe(1);
    });

    it("inAnyState matches any listed state", async () => {
        const cond = inAnyState(MachineState.IDLE, MachineState.PAUSED);
        expect(cond(st(MachineState.PAUSED))).toBe(true);
        expect(cond(st(MachineState.IDLE))).toBe(true);
        expect(cond(st(MachineState.RUNNING))).toBe(false);
    });
});

describe("waitAtRest", () => {
    it("resolves true when the machine settles", async () => {
        const src = source([st(MachineState.RUNNING, 1), st(MachineState.IDLE)]);
        expect(await waitAtRest(src, 5000, clock())).toBe(true);
    });

    it("resolves false instead of throwing when it does not", async () => {
        const src = source([st(MachineState.RUNNING, 1)]);
        expect(await waitAtRest(src, 500, clock())).toBe(false);
    });

    it("counts ALARM as rest — the machine is stationary", async () => {
        // Preserves the pre-existing waitIdle behaviour, which returned true
        // here. A caller that wants ALARM to fail should use settle() directly.
        const src = source([st(MachineState.ALARM, 0)]);
        expect(await waitAtRest(src, 5000, clock())).toBe(true);
    });
});
