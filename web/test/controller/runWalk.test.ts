/**
 * runWalk.test.ts — driving a baked walk at a machine that exists in time.
 *
 * The events here are hand-built rather than baked from an SVG, because what is
 * under test is not the geometry — it is the four rules that separate "the
 * stream returned" from "the machine stopped", and each of them is provoked by
 * a specific flag on a specific segment. A real bake would bury them.
 */

import { describe, it, expect } from "vitest";
import { Controller } from "../../src/controller/controller.js";
import { runWalk } from "../../src/controller/runWalk.js";
import { Link } from "../../src/wire/link/link.js";
import { SimTransport } from "../../src/wire/link/backends/sim.js";
import {
    microSegment,
    MICRO_PAUSE,
    MICRO_LIFT,
    MICRO_DUTY_RELEASE,
    MICRO_DUTY_ASSERT,
} from "../../src/wire/format/microsegment.js";
import { unpackMicrosegment } from "../../src/wire/format/packet.js";
import {
    PACKET_SIZE,
    MAGIC_MICROSEG,
    MAGIC_JOG,
} from "../../src/wire/format/constants.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    ToolType,
} from "../../src/machine/schema.js";
import { KNIFE, PEN } from "../../src/machine/tools.js";
import { MachineState } from "../../src/wire/format/status.js";
import { AbortFlag } from "../../src/wire/link/transport.js";
import type { WalkEvent } from "../../src/orchestrate/walk.js";

function dualHead() {
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        [
            toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667, { rotary: true }), {
                profile: KNIFE,
            }),
            toolHead(axisConfig(busNode(5), 600), axisConfig(busNode(6), 51.667, { rotary: true }), {
                profile: PEN,
                xOffset: 50,
            }),
        ],
    );
}

/**
 * A Sim that keeps every motion packet it was handed. The sim consumes its own
 * motion queue as it executes, so by the time a run returns there is nothing
 * left to inspect — and the flags on individual segments are exactly what these
 * tests are about.
 */
class RecordingSim extends SimTransport {
    readonly packets: Uint8Array[] = [];

    override write(bytes: Uint8Array): Promise<void> {
        // On the wire a batch is bare 26-byte packets back to back — the u16
        // length prefix belongs to the .plan FILE format, not the port. Text
        // lines share the same channel, so gate on the magic.
        for (let off = 0; off + PACKET_SIZE <= bytes.length; off += PACKET_SIZE) {
            const magic = bytes[off];
            if (magic !== MAGIC_MICROSEG && magic !== MAGIC_JOG) break;
            this.packets.push(bytes.slice(off, off + PACKET_SIZE));
        }
        return super.write(bytes);
    }
}

async function bench() {
    const machine = dualHead();
    const sim = new RecordingSim({ busNodes: [1, 2, 3, 4, 5, 6], frameMs: 2, ringSize: 256 });
    const link = new Link(sim);
    const controller = new Controller(machine, link, { pollMs: 2 });
    await controller.commit(0);
    return { machine, sim, link, controller };
}

/** Segments that write nothing interesting — just enough to make the sim move. */
const motion = (n: number, flags = 0): WalkEvent => ({
    kind: "motion",
    segments: Array.from({ length: n }, (_, i) =>
        microSegment(1, 0, 0, 0, 2000, i === n - 1 ? flags : 0),
    ),
});

const pause = (swapIn: ToolType[], swapOut: ToolType[] = []): WalkEvent => ({
    kind: "pause",
    swapIn,
    swapOut,
    mount: swapIn,
});

/** Every MicroSegment the sim was actually sent, in order. */
function segmentsSeen(sim: RecordingSim): ReturnType<typeof unpackMicrosegment>[] {
    return sim.packets.map((p) => unpackMicrosegment(p));
}

// ── pre-flight ───────────────────────────────────────────────────────────────

describe("pre-flight", () => {
    it("refuses to run against a machine whose axis map does not match", async () => {
        const { controller } = await bench();
        controller.engage(1); // pending, not committed
        await expect(runWalk(controller, [motion(4)])).rejects.toThrow(/axis map/);
    });

    it("refuses to run from a state that would NACK every packet", async () => {
        const { controller, sim } = await bench();
        sim.state = MachineState.ALARM;
        await expect(runWalk(controller, [motion(4)])).rejects.toThrow(/ALARM/);
    });

    it("holds the job lease for the whole run and releases it after", async () => {
        const { controller } = await bench();
        const busyDuring: (string | null)[] = [];
        controller.on("busy", (b) => busyDuring.push(b));
        await runWalk(controller, [motion(4)]);
        expect(busyDuring).toEqual(["job", null]);
        expect(controller.busy).toBeNull();
    });

    it("refuses to start while something else holds the lease", async () => {
        const { controller } = await bench();
        controller.acquire("jog");
        await expect(runWalk(controller, [motion(4)])).rejects.toThrow(/busy: jog/);
    });
});

// ── the four rules ───────────────────────────────────────────────────────────

describe("streaming", () => {
    it("coalesces consecutive motion events and reports progress against the total", async () => {
        const { controller } = await bench();
        const progress: number[] = [];
        const result = await runWalk(controller, [motion(3), motion(2), motion(4)], {
            onProgress: (sent) => progress.push(sent),
        });
        expect(result.segmentsTotal).toBe(9);
        expect(result.segmentsSent).toBe(9);
        // One stream, not three: the batching is what keeps the window full.
        expect(progress).toEqual([9]);
    });

    it("waits for the machine, not for the writer", async () => {
        // Rule 1: the host is acked ahead of the Pico by the depth of its ring.
        // If the runner returned on the ack, the sim would still be executing.
        const { controller, sim } = await bench();
        await runWalk(controller, [motion(20)]);
        expect(sim.state).toBe(MachineState.IDLE);
        expect(sim.pos[0]).toBe(20);
    });

    it("stamps MICRO_PAUSE on the last segment before a swap, then resumes", async () => {
        // Rule 2: without the stamp the machine runs straight on into the tool
        // change instead of parking for it.
        const { controller, sim } = await bench();
        let promptedWhileParked: number | null = null;

        const result = await runWalk(controller, [motion(4), pause([KNIFE.toolType]), motion(3)], {
            confirmSwap: () => {
                promptedWhileParked = sim.state;
                return true;
            },
        });

        expect(result.pauses).toBe(1);
        expect(promptedWhileParked).toBe(MachineState.PAUSED);
        const seen = segmentsSeen(sim);
        expect(seen[3]!.flags & MICRO_PAUSE).toBeTruthy();
        // ...and only there — a stamp on a mid-batch segment would park the
        // machine in the middle of a cut.
        expect(seen.filter((s) => s.flags & MICRO_PAUSE)).toHaveLength(1);
        expect(sim.state).toBe(MachineState.IDLE);
        expect(result.segmentsSent).toBe(7);
    });

    it("abandons the run when the operator declines the swap", async () => {
        const { controller } = await bench();
        await expect(
            runWalk(controller, [motion(2), pause([KNIFE.toolType])], { confirmSwap: () => false }),
        ).rejects.toThrow(/cancelled by the operator/);
    });
});

describe("duty breaks", () => {
    // Rule 3: a duty break is baked INTO the segments, so it can land anywhere
    // in a batch. Streaming past it means the firmware parks at that segment and
    // NACKs the whole remainder.
    const broken = (): WalkEvent => ({
        kind: "motion",
        segments: [
            microSegment(1, 0, 0, 0, 2000),
            microSegment(1, 0, 0, 0, 2000),
            // The invariant from microsegment.ts: the markers are meaningless
            // without MICRO_PAUSE, because the relay needs a free bus.
            microSegment(1, 0, 0, 0, 2000, MICRO_LIFT | MICRO_PAUSE | MICRO_DUTY_RELEASE | MICRO_DUTY_ASSERT),
            microSegment(1, 0, 0, 0, 2000),
            microSegment(1, 0, 0, 0, 2000),
        ],
    });

    it("cuts the batch at the marker, relays, then carries on from there", async () => {
        const { controller, sim } = await bench();
        const stateAtBreak: number[] = [];

        const result = await runWalk(controller, [broken()], {
            onDutyBreak: () => {
                stateAtBreak.push(sim.state);
            },
        });

        // The bus is only free once the firmware has actually parked.
        expect(stateAtBreak).toEqual([MachineState.PAUSED]);
        expect(result.segmentsSent).toBe(5);
        expect(sim.pos[0]).toBe(5);
        expect(sim.state).toBe(MachineState.IDLE);
    });

    it("passes the phase's live tool set to the hook, not the plan's", async () => {
        // dutyLimits is config, not preset — the runner cannot resolve the
        // peripheral itself, so it hands over what is cutting right now.
        const { controller } = await bench();
        const seen: ToolType[][] = [];
        await runWalk(controller, [pause([PEN.toolType]), broken()], {
            confirmSwap: () => true,
            onDutyBreak: (mount) => {
                seen.push([...mount]);
            },
        });
        expect(seen).toEqual([[PEN.toolType]]);
    });
});

describe("head rebinding", () => {
    it("rebinds the slots to the incoming tool's head before the prompt", async () => {
        // Rule 4: the walk has already switched heads; if the map does not
        // follow, the next block drives the new head's Z/A through the old
        // head's motors.
        const { controller, sim } = await bench();
        let mapAtPrompt: readonly (number | null)[] = [];

        await runWalk(controller, [motion(2), pause([PEN.toolType], [KNIFE.toolType]), motion(2)], {
            confirmSwap: (req) => {
                mapAtPrompt = [...sim.slotNode];
                expect(req.head).toBe(1);
                return true;
            },
        });

        // Head 1's Z/A are nodes 5 and 6 — bound before the operator was asked.
        expect(mapAtPrompt).toEqual([1, 2, 5, 6]);
        expect(controller.setup.engaged).toBe(1);
        expect(controller.synced).toBe(true);
    });

    it("acts on a rebind event with no operator involved", async () => {
        // The mid-phase case: the walk changed heads between blocks, so there
        // is no pause to hang the rebind off. Head 1's Z/A are nodes 5 and 6.
        const { controller, sim } = await bench();
        const mapDuring: (number | null)[][] = [];

        await runWalk(controller, [
            motion(2),
            { kind: "rebind", head: 1 },
            motion(2),
        ], {
            confirmSwap: () => {
                throw new Error("a rebind must not prompt the operator");
            },
            onProgress: () => mapDuring.push([...sim.slotNode]),
        });

        expect(sim.slotNode).toEqual([1, 2, 5, 6]);
        expect(controller.setup.engaged).toBe(1);
        expect(controller.synced).toBe(true);
        // Bound between the two batches, not before both of them.
        expect(mapDuring).toEqual([[1, 2, 3, 4], [1, 2, 5, 6]]);
    });

    it("leaves the map alone when the swap does not change heads", async () => {
        const { controller, sim } = await bench();
        await runWalk(controller, [pause([KNIFE.toolType])], {
            confirmSwap: (req) => {
                expect(req.head).toBeNull();
                return true;
            },
        });
        expect(sim.slotNode).toEqual([1, 2, 3, 4]);
    });
});

describe("phases and abort", () => {
    it("announces the opening tool set even when no pause opens the job", async () => {
        // A walk only emits a pause where something is SWAPPED, so a schedule
        // whose first phase needs no swap would otherwise start cutting with
        // nothing armed.
        const { controller } = await bench();
        const phases: (readonly ToolType[] | null)[] = [];
        await runWalk(controller, [motion(3)], {
            initialMount: [KNIFE.toolType],
            onPhase: (mount) => {
                phases.push(mount);
            },
        });
        expect(phases).toEqual([[KNIFE.toolType]]);
    });

    it("stops at an event boundary when aborted, and says so rather than reporting success", async () => {
        const { controller, sim } = await bench();
        const flag = new AbortFlag();
        flag.set();
        const result = await runWalk(controller, [motion(2), pause([KNIFE.toolType]), motion(2)], {
            abort: flag,
            confirmSwap: () => true,
        });
        expect(result.aborted).toBe(true);
        expect(result.segmentsSent).toBe(0);
        expect(result.segmentsTotal).toBe(4);
        expect(sim.packets).toHaveLength(0);
    });

    it("does not mutate the caller's event list", async () => {
        // The duty-break split re-queues the tail of a batch; doing that to the
        // caller's array would corrupt a walk they may want to re-run.
        const { controller } = await bench();
        const events = [
            {
                kind: "motion" as const,
                segments: [
                    microSegment(1, 0, 0, 0, 2000),
                    microSegment(1, 0, 0, 0, 2000, MICRO_PAUSE | MICRO_DUTY_RELEASE),
                    microSegment(1, 0, 0, 0, 2000),
                ],
            },
        ];
        const before = events.length;
        await runWalk(controller, events, { onDutyBreak: () => {} });
        expect(events.length).toBe(before);
        expect(events[0]!.segments).toHaveLength(3);
    });
});
