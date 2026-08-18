/**
 * controller/runWalk.ts — stream a walk to the machine.
 *
 * `walkSchedule()` produces the events; this drives them. The gap between the
 * two is not bookkeeping, it is four rules that every host has to get right and
 * that nothing in the bake tower can express, because they are all about a
 * machine that exists in time:
 *
 *   1. The host is acked AHEAD of the Pico by the depth of its ring buffer. The
 *      last packet is confirmed long before the last step is taken, so "the
 *      stream returned" is not "the machine stopped". Anything gated on state
 *      must wait for the machine.
 *
 *   2. A pause is a barrier in both directions. The last segment before a swap
 *      carries MICRO_PAUSE so the firmware parks itself rather than running on
 *      into a tool change; the host then has to notice it parked, do the swap,
 *      and resume.
 *
 *   3. A duty break is baked INTO the segments (compileBlock stage 9), not
 *      signalled by a walk event, so it can land anywhere inside a batch.
 *      Streaming past it is not an option — the firmware parks at that segment
 *      and NACKs the whole remainder with NACK_PAUSED.
 *
 *   4. On a dual-head machine a tool swap is also a SLOT REBIND. The walk has
 *      switched to the head holding the incoming tool; if the axis map does not
 *      follow, the next block drives the new head's Z/A through the old head's
 *      motors.
 *
 * Peripheral policy is deliberately NOT here. Which node runs the knife
 * oscillator, and whether the vacuum belongs to the job or the shop, is
 * installation knowledge — the hooks hand the caller each boundary where the bus
 * is free (the firmware refuses peripheral relays while RUNNING) and let it
 * decide.
 */

import type { WalkEvent } from "../orchestrate/walk.js";
import type { MountSet } from "../orchestrate/schedule.js";
import type { MicroSegment } from "../wire/format/microsegment.js";
import {
    MICRO_PAUSE,
    MICRO_DUTY_ASSERT,
    MICRO_DUTY_RELEASE,
} from "../wire/format/microsegment.js";
import { packMicrosegment } from "../wire/format/packet.js";
import { fatalReasonName, DEFAULT_WINDOW } from "../wire/link/session.js";
import { inState } from "../wire/link/settled.js";
import { MachineState } from "../wire/format/status.js";
import { stateName } from "../wire/format/names.js";
import type { AbortToken } from "../wire/link/transport.js";
import type { Controller } from "./controller.js";

/** A pause event, as handed to `confirmSwap`. */
export interface SwapRequest {
    readonly swapIn: MountSet;
    readonly swapOut: MountSet;
    /** The full tool set in force for the phase this pause opens. */
    readonly mount: MountSet;
    /** Head the map was rebound to for this phase, or null if no rebind was needed. */
    readonly head: number | null;
}

export interface RunWalkHooks {
    /**
     * Ask the operator to make the swap. Return false to abandon the run.
     *
     * Called AFTER the axis map has been rebound, so by the time the prompt is
     * on screen the machine is already bound to the head the operator is about
     * to fit a tool into.
     */
    confirmSwap?(req: SwapRequest): Promise<boolean> | boolean;
    /**
     * A phase boundary — the machine is provably at rest and the bus is free.
     * The one window where a peripheral relay is accepted. `mount` is the tool
     * set about to cut; `null` means teardown.
     */
    onPhase?(mount: MountSet | null, why: string): Promise<void> | void;
    /**
     * A baked duty break: the machine is PAUSED at a lift, the blade is clear.
     * The caller releases, waits out the dwell, re-asserts. Throwing here aborts
     * the run, which is correct — resuming would plunge a dead tool.
     */
    onDutyBreak?(mount: MountSet): Promise<void> | void;
    /** Segments confirmed so far, out of the total. For a progress bar. */
    onProgress?(sent: number, total: number): void;
    /** Narration. `kind` mirrors the demo console's classes. */
    onLog?(message: string, kind: "note" | "tx" | "ok" | "err"): void;
    /** Checked at every event boundary; set it to stop between batches. */
    abort?: AbortToken;
    /** Go-Back-N window. Default 16. */
    window?: number;
    /**
     * Tools in force before the first pause — `schedule.phases[0].mount`.
     *
     * Needed because a walk only emits a pause where something is swapped, so a
     * schedule whose first phase needs no swap starts cutting with no event to
     * announce what it is cutting with. Without this the opening `onPhase` would
     * arm nothing and the first blade would drag cold.
     */
    initialMount?: MountSet;
}

export interface RunWalkResult {
    readonly segmentsSent: number;
    readonly segmentsTotal: number;
    readonly pauses: number;
    /** True if `abort` cut the run short rather than it finishing. */
    readonly aborted: boolean;
}

/**
 * Stream `events` under a "job" lease.
 *
 * Throws on a stream failure, an operator cancellation, or a refused rebind —
 * the caller's `finally` is where peripheral teardown belongs, because a
 * teardown that throws must not replace the error that caused it.
 */
export async function runWalk(
    controller: Controller,
    events: readonly WalkEvent[],
    hooks: RunWalkHooks = {},
): Promise<RunWalkResult> {
    const { onLog, onProgress, abort, window = DEFAULT_WINDOW } = hooks;
    const log = (m: string, k: "note" | "tx" | "ok" | "err" = "note"): void => onLog?.(m, k);

    // Pre-flight. Both of these produce the same symptom if skipped — every
    // packet NACKed — and neither symptom names its cause, so refusing up front
    // is far kinder than a stream that dies 16 packets in.
    if (!controller.synced) {
        throw new Error(
            "the firmware's axis map does not match this setup — commit it before running",
        );
    }
    const pre = await controller.refresh();
    if (pre.state !== MachineState.IDLE && pre.state !== MachineState.PAUSED) {
        throw new Error(
            `machine is ${stateName(pre.state)} — unalarm or commit an axis map first`,
        );
    }

    return controller.withLease("job", async () => {
        // A local copy: the duty-break split re-inserts the tail of a batch as a
        // fresh event, and doing that to the caller's array would mutate a value
        // they may well render or re-run.
        const queue: WalkEvent[] = [...events];
        const total = countSegments(queue);
        let sent = 0;
        let pauses = 0;
        let aborted = false;

        // Which tools are live right now. A duty break needs the profile whose
        // dutyLimits produced it, and the segments themselves carry only timing.
        let activeMount: MountSet = hooks.initialMount ?? firstMount(queue);
        await hooks.onPhase?.(activeMount, "job start");

        let i = 0;
        let machinePaused = false;

        while (i < queue.length) {
            if (abort?.isSet()) {
                aborted = true;
                break;
            }

            const ev = queue[i]!;

            if (ev.kind === "pause") {
                pauses++;
                const head = await rebindForSwap(controller, ev.swapIn, log);
                const ok = await hooks.confirmSwap?.({
                    swapIn: ev.swapIn,
                    swapOut: ev.swapOut,
                    mount: ev.mount,
                    head,
                });
                if (ok === false) throw new Error("cancelled by the operator at the tool swap");

                // After the tool is physically in and before anything moves —
                // the machine is PAUSED here, the one window the gate allows.
                await hooks.onPhase?.(ev.mount, "phase change");
                activeMount = ev.mount;

                if (machinePaused) {
                    await resume(controller, log);
                    machinePaused = false;
                }
                i++;
                continue;
            }

            // Coalesce consecutive motion events into one stream.
            const batch: MicroSegment[] = [];
            while (i < queue.length && queue[i]!.kind === "motion") {
                batch.push(...(queue[i] as Extract<WalkEvent, { kind: "motion" }>).segments);
                i++;
            }
            if (batch.length === 0) continue;

            // Rule 3: cut the batch at the first duty marker and re-queue the
            // tail, so the run resumes from exactly where the firmware parked.
            const brk = batch.findIndex((s) => s.flags & (MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT));
            if (brk >= 0 && brk < batch.length - 1) {
                queue.splice(i, 0, { kind: "motion", segments: batch.slice(brk + 1) });
                batch.length = brk + 1;
            }
            const dutyBreak = brk >= 0;
            if (dutyBreak) machinePaused = true;

            // Rule 2: stamp MICRO_PAUSE on the last segment before a swap so the
            // machine parks itself instead of running on into the tool change.
            const nextIsPause = !dutyBreak && i < queue.length && queue[i]!.kind === "pause";
            if (nextIsPause) {
                const last = batch[batch.length - 1]!;
                batch[batch.length - 1] = { ...last, flags: last.flags | MICRO_PAUSE };
                machinePaused = true;
            }

            log(`streaming ${batch.length} segments`, "tx");
            const result = await controller.link.stream(
                batch.map((s, n) => packMicrosegment(s, n & 0xff)),
                window,
            );
            if (!result.ok) {
                const why =
                    result.fatalReason !== undefined ? fatalReasonName(result.fatalReason) : "unknown";
                throw new Error(
                    `stream failed: ${why} — emitted ${result.emitted} acked ${result.acked} ` +
                    `nacks ${result.nacks}`,
                );
            }

            sent += batch.length;
            onProgress?.(sent, total);

            // Rule 1: the ack said the Pico HAS the packets, not that it has run
            // them. Wait for the machine.
            await controller.settle(
                inState(nextIsPause || dutyBreak ? MachineState.PAUSED : MachineState.IDLE),
            );

            if (dutyBreak) {
                await hooks.onDutyBreak?.(activeMount);
                await resume(controller, log);
                machinePaused = false;
            }
        }

        if (!aborted) log(`done — ${sent} segments streamed`, "ok");
        return { segmentsSent: sent, segmentsTotal: total, pauses, aborted };
    });
}

// ── helpers ──────────────────────────────────────────────────────────────────

/**
 * Rule 4. If the incoming tools live on a head the firmware is not bound to,
 * rebind before anything else moves. Returns the head rebound to, or null when
 * the map was already right.
 *
 * The wait matters: `commit` refuses while RUNNING, and after a MICRO_PAUSE the
 * machine may still be ramping down when this runs.
 */
async function rebindForSwap(
    controller: Controller,
    swapIn: MountSet,
    log: (m: string, k?: "note" | "tx" | "ok" | "err") => void,
): Promise<number | null> {
    const assign = controller.headAssignment;
    let want: number | undefined;
    for (const t of swapIn) {
        const h = assign.get(t);
        if (h !== undefined) {
            want = h;
            break;
        }
    }
    const head = want ?? controller.setup.engaged;

    // No head change AND the firmware already agrees: nothing owed. The second
    // half is not redundant — a map can drift out of sync without any head
    // switch (another host, a Pico reboot mid-job), and streaming the next block
    // against a stale map is the failure this rebind exists to prevent.
    if (head === controller.setup.engaged && controller.synced) return null;

    if (!(await controller.waitAtRest())) {
        throw new Error("machine did not come to rest for the slot rebind");
    }
    log(`rebinding slots to head ${head}`, "note");
    await controller.commit(head);
    return head;
}

async function resume(
    controller: Controller,
    log: (m: string, k?: "note" | "tx" | "ok" | "err") => void,
): Promise<void> {
    const reply = await controller.link.command("resume");
    log(`resume → ${reply}`, reply === "ok" ? "ok" : "err");
    if (reply !== "ok") throw new Error(`resume refused: ${reply || "no reply"}`);
}

function countSegments(events: readonly WalkEvent[]): number {
    let n = 0;
    for (const e of events) if (e.kind === "motion") n += e.segments.length;
    return n;
}

function firstMount(events: readonly WalkEvent[]): MountSet {
    for (const e of events) if (e.kind === "pause") return e.mount;
    return [];
}
