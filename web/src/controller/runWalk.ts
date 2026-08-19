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
 *   4. On a dual-head machine the axis map has to follow the walk. The walk
 *      says WHERE with a `rebind` event carrying the head; if the map does not
 *      follow, the next block drives the new head's Z/A through the old head's
 *      motors. Nothing here reconstructs that head from the tools — the head a
 *      block was compiled against is the only one its segments are valid for.
 *
 *   5. An operator saying "done" is not evidence. What is actually fitted is
 *      checked before the first segment and again after every swap, because a
 *      tool in the wrong socket is a silent 2x Z error, not a failure.
 *
 * Peripheral policy is deliberately NOT here. Which node runs the knife
 * oscillator, and whether the vacuum belongs to the job or the shop, is
 * installation knowledge — the hooks hand the caller each boundary where the bus
 * is free (the firmware refuses peripheral relays while RUNNING) and let it
 * decide.
 */

import type { WalkEvent } from "../orchestrate/walk.js";
import type { Mounts } from "../production/schedule.js";
import type { ToolType } from "../machine/index.js";
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
import { verifyPhaseMounts } from "./controller.js";

/** A pause event, as handed to `confirmSwap`. */
export interface SwapRequest {
    readonly swapIn: readonly ToolType[];
    readonly swapOut: readonly ToolType[];
    /** The full tool set in force for the phase this pause opens. */
    readonly mounts: Mounts;
}

export interface RunWalkHooks {
    /**
     * Ask the operator to make the swap. Return false to abandon the run.
     *
     * What the operator did is CHECKED when this returns: `true` is a human
     * claim, and the whole point of stage 5 is that a mis-mount becomes a
     * refusal rather than a wrong cut.
     */
    confirmSwap?(req: SwapRequest): Promise<boolean> | boolean;
    /**
     * A phase boundary — the machine is provably at rest and the bus is free.
     * The one window where a peripheral relay is accepted. `mount` is the tool
     * set about to cut; `null` means teardown.
     */
    onPhase?(mounts: Mounts | null, why: string): Promise<void> | void;
    /**
     * A baked duty break: the machine is PAUSED at a lift, the blade is clear.
     * The caller releases, waits out the dwell, re-asserts. Throwing here aborts
     * the run, which is correct — resuming would plunge a dead tool.
     */
    onDutyBreak?(mounts: Mounts): Promise<void> | void;
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
    initialMount?: Mounts;
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
        let activeMount: Mounts = hooks.initialMount ?? firstMount(queue);

        // Rule 5, before any material moves. The check is per PHASE, not per
        // job: a swap job's later blocks are SUPPOSED to be unmounted right now
        // — that is what the pause is for — so verifying every block up front
        // would refuse every multi-tool job on a single-head machine. Each
        // phase is checked as it opens, here for the first and after every
        // confirmSwap for the rest, which covers the same ground at the moment
        // each claim can actually be true.
        verifyPhaseMounts(activeMount, controller.setup);
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
                const ok = await hooks.confirmSwap?.({
                    swapIn: ev.swapIn,
                    swapOut: ev.swapOut,
                    mounts: ev.mounts,
                });
                if (ok === false) throw new Error("cancelled by the operator at the tool swap");

                // Rule 5. The machine just changed under us; re-check it.
                verifyPhaseMounts(ev.mounts, controller.setup);

                // After the tool is physically in and before anything moves —
                // the machine is PAUSED here, the one window the gate allows.
                await hooks.onPhase?.(ev.mounts, "phase change");
                activeMount = ev.mounts;

                if (machinePaused) {
                    await resume(controller, log);
                    machinePaused = false;
                }
                i++;
                continue;
            }

            if (ev.kind === "rebind") {
                // A swap minus the operator: the walk changed heads mid-phase
                // and the map must follow before the next segment drives a Z or
                // an A. No MICRO_PAUSE is needed to get here — the batch before
                // a rebind is not followed by a pause, so it already settled to
                // IDLE, and IDLE is at rest.
                if (!(await controller.waitAtRest())) {
                    throw new Error("machine did not come to rest for the slot rebind");
                }
                log(`rebinding slots to head ${ev.head}`, "note");
                await controller.commit(ev.head);
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

function firstMount(events: readonly WalkEvent[]): Mounts {
    for (const e of events) if (e.kind === "pause") return e.mounts;
    return [];
}
