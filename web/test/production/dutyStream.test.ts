/**
 * A duty break has to survive the WIRE, not just the planner: the marked
 * segment carries MICRO_PAUSE, so the machine parks in PAUSED partway through
 * a job and the host has to resume and stream the remainder.
 *
 * These drive the Sim the way the demo's runner does — stream up to the marker,
 * wait for PAUSED, resume, stream the rest — because that round trip is where
 * the flag stops being a planning detail and starts being a protocol one.
 */

import { describe, it, expect } from "vitest";
import { Link } from "../../src/wire/link/link.js";
import { SimTransport } from "../../src/wire/link/backends/sim.js";
import { packMicrosegment } from "../../src/wire/format/packet.js";
import {
    microSegment,
    MICRO_LIFT,
    MICRO_PAUSE,
    MICRO_DUTY_RELEASE,
    MICRO_DUTY_ASSERT,
    type MicroSegment,
} from "../../src/wire/format/microsegment.js";
import { MachineState } from "../../src/wire/format/status.js";

const F_CPU = 1_000_000;

async function withLink<T>(fn: (link: Link) => Promise<T>): Promise<T> {
    const sim = new SimTransport({ axisMap: [1, 2, 3, 4], frameMs: 5, fCpu: F_CPU });
    const link = new Link(sim);
    const poller = setInterval(() => void link.getStatus().catch(() => undefined), 20);
    try {
        return await fn(link);
    } finally {
        clearInterval(poller);
        await link.close();
    }
}

async function waitState(link: Link, want: MachineState, ms = 4000): Promise<MachineState> {
    const dl = Date.now() + ms;
    let last: MachineState = MachineState.IDLE;
    while (Date.now() < dl) {
        last = (await link.getStatus()).state;
        if (last === want) return last;
        await new Promise((r) => setTimeout(r, 10));
    }
    return last;
}

/** Short X move, ~1 ms each so a batch runs fast. */
const seg = (flags = 0): MicroSegment => microSegment(10, 0, 0, 0, 100, flags);

const send = (link: Link, segs: readonly MicroSegment[]) =>
    link.stream(segs.map((s, n) => packMicrosegment(s, n & 0xff)), 16);

describe("duty break over the wire", () => {
    it("a marked segment parks the machine in PAUSED", { timeout: 15000 }, async () => {
        await withLink(async (link) => {
            const batch = [
                ...Array.from({ length: 20 }, () => seg()),
                seg(MICRO_LIFT | MICRO_PAUSE | MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT),
            ];
            const r = await send(link, batch);
            expect(r.ok).toBe(true);
            expect(await waitState(link, MachineState.PAUSED)).toBe(MachineState.PAUSED);
        });
    });

    it("resume then stream the remainder — position is continuous across the break",
        { timeout: 15000 }, async () => {
        await withLink(async (link) => {
            const first = [
                ...Array.from({ length: 20 }, () => seg()),
                seg(MICRO_LIFT | MICRO_PAUSE | MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT),
            ];
            expect((await send(link, first)).ok).toBe(true);
            expect(await waitState(link, MachineState.PAUSED)).toBe(MachineState.PAUSED);

            const mid = await link.getStatus();
            expect(mid.pos![0]).toBe(210); // 21 segments x 10 steps

            expect(await link.command("resume")).toBe("ok");

            const rest = Array.from({ length: 20 }, () => seg());
            expect((await send(link, rest)).ok).toBe(true);
            expect(await waitState(link, MachineState.IDLE)).toBe(MachineState.IDLE);

            const end = await link.getStatus();
            expect(end.pos![0]).toBe(410); // nothing lost across the pause
        });
    });

    it("the runner's split/resume loop drains a multi-break job to IDLE",
        { timeout: 30000 }, async () => {
        // Mirrors the demo runner's loop (comms.js): batch motion events, cut
        // the batch at the first duty marker, push the remainder back as a new
        // event, stream, wait for PAUSED, toggle, resume, continue. The question
        // this answers is whether that loop TERMINATES with several breaks —
        // splicing into the array you are iterating is exactly where it wouldn't.
        const mark = MICRO_LIFT | MICRO_PAUSE | MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT;
        const run = Array.from({ length: 30 }, () => seg());
        const all = [
            ...run, seg(mark),
            ...run, seg(mark),
            ...run, seg(mark),
            ...run,
        ];

        await withLink(async (link) => {
            const events: { kind: "motion"; segments: MicroSegment[] }[] =
                [{ kind: "motion", segments: all }];
            let i = 0, breaks = 0, sent = 0;

            while (i < events.length) {
                const batch: MicroSegment[] = [];
                while (i < events.length && events[i]!.kind === "motion") {
                    batch.push(...events[i]!.segments); i++;
                }
                const brk = batch.findIndex(
                    (s) => s.flags & (MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT),
                );
                if (brk >= 0 && brk < batch.length - 1) {
                    events.splice(i, 0, { kind: "motion", segments: batch.slice(brk + 1) });
                    batch.length = brk + 1;
                }

                expect((await send(link, batch)).ok).toBe(true);
                sent += batch.length;

                const want = brk >= 0 ? MachineState.PAUSED : MachineState.IDLE;
                expect(await waitState(link, want, 8000)).toBe(want);
                if (brk >= 0) {
                    breaks++;
                    expect(await link.command("resume")).toBe("ok");
                }
            }

            expect(breaks).toBe(3);
            expect(sent).toBe(all.length);
            expect((await link.getStatus()).state).toBe(MachineState.IDLE);
            expect((await link.getStatus()).pos![0]).toBe(all.length * 10);
        });
    });

    it("a batch longer than the ring still completes", { timeout: 20000 }, async () => {
        // The demo streams hundreds of segments per batch against a 64-deep
        // ring, so the sender must ride backpressure all the way to a marker
        // sitting well past the ring's end.
        await withLink(async (link) => {
            const batch = [
                ...Array.from({ length: 300 }, () => seg()),
                seg(MICRO_LIFT | MICRO_PAUSE | MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT),
            ];
            const r = await send(link, batch);
            expect(r.ok).toBe(true);
            expect(await waitState(link, MachineState.PAUSED, 8000)).toBe(MachineState.PAUSED);
            expect((await link.getStatus()).pos![0]).toBe(3010);
        });
    });
});
