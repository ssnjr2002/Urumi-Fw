/**
 * controller/prepareZ.ts — put the engaged head's Z at clear height.
 *
 * A compiled block starts and ends at clear height (its lift is relative), so
 * before a phase's first block the runner calls this
 * (docs/tool_probe_planner_integration.md §4.3):
 *
 *   1. If the Pico holds no height for this Z, measure one:
 *      - with a probe block: raise Z to its homing park, move XY over the
 *        switch, probe, raise Z again and return XY to where it was. XY travels
 *        only at the homing park, since the material height near the switch is
 *        unknown. The Pico stores the switch contact.
 *      - without one: `touchOff` hands the machine to the operator, who jogs
 *        the tip onto the mat and stores that Z (`touchOffHere`). XY is
 *        recorded before and restored after, at clear height.
 *   2. Move Z to clear, clearanceMm above the material.
 */

import type { MachineConfig, ToolProfile, AxisConfig } from "../machine/schema.js";
import { toolHeights, zAtHeightSteps } from "../machine/heights.js";
import { toolFrameOffset, toolToHome, stepsToUnits } from "../machine/frames.js";
import { derivePlan } from "../homing/derive.js";
import { deriveProbePlan } from "../probe/derive.js";
import { runProbe, type RunProbeOptions } from "../probe/sequence.js";
import { jogToPoint, type JogTarget } from "../operatorJog/jogTo.js";
import { getPos, getState, setProbe, type ProbeReply } from "../wire/link/commands.js";
import type { Link } from "../wire/link/link.js";
import type { Controller } from "./controller.js";

export interface PrepareZOptions {
    probe?: RunProbeOptions;
    /**
     * For a Z with no probe block: resolves once the operator has jogged the
     * tip onto the mat and stored it (`touchOffHere`), i.e. `probed=1`. Throw
     * to abort.
     */
    touchOff?(head: number, tool: ToolProfile): Promise<void>;
    onLog?(message: string): void;
}

async function moveTo(
    controller: Controller,
    targets: readonly JogTarget[],
    rate: number,
    what: string,
): Promise<void> {
    // A jog session stamps seq from 0.
    if (!(await controller.link.resetSeq())) throw new Error(`${what}: seq reset failed`);
    if (!(await jogToPoint(controller.link, targets, rate).done)) {
        throw new Error(`${what}: the move failed`);
    }
    if (!(await controller.waitAtRest())) throw new Error(`${what}: machine did not come to rest`);
}

/** Store the engaged Z's current position as its mat height. */
export async function touchOffHere(link: Link): Promise<ProbeReply> {
    return setProbe(link, (await getPos(link))[2]);
}

function zTarget(z: AxisConfig, wireSteps: number): JogTarget {
    return { axisIndex: 2, axis: z, targetPos: stepsToUnits(wireSteps, z) };
}

async function moveXY(
    controller: Controller,
    machine: MachineConfig,
    xSteps: number,
    ySteps: number,
    what: string,
): Promise<void> {
    await moveTo(controller, [
        { axisIndex: 0, axis: machine.x, targetPos: stepsToUnits(xSteps, machine.x) },
        { axisIndex: 1, axis: machine.y, targetPos: stepsToUnits(ySteps, machine.y) },
    ], machine.rapid.feed, what);
}

/** Probe on the bed switch. Leaves Z at its homing park and XY where it was. */
async function probeOnSwitch(
    controller: Controller,
    machine: MachineConfig,
    head: number,
    profile: ToolProfile,
    opts: PrepareZOptions,
): Promise<number> {
    const z = machine.heads[head]!.z;
    const probe = z.probe!;
    if (z.homing?.kind !== "linear") {
        throw new Error(`head ${head}: Z has no linear homing, so no safe height to travel at`);
    }
    opts.onLog?.(`probing head ${head} (${profile.name})`);
    const [x0, y0] = await getPos(controller.link);

    const park = zTarget(z, derivePlan("z", z).datumSteps);
    await moveTo(controller, [park], machine.z.feed, "raise Z to park");
    const sw = toolToHome({ x: probe.switchXMm, y: probe.switchYMm },
        toolFrameOffset(machine, head, profile));
    await moveTo(controller, [
        { axisIndex: 0, axis: machine.x, targetPos: sw.x },
        { axisIndex: 1, axis: machine.y, targetPos: sw.y },
    ], machine.rapid.feed, "move over the switch");

    const contact = await runProbe(controller.link, deriveProbePlan(z), opts.probe);
    opts.onLog?.(`head ${head} contact at Z ${contact} steps`);

    await moveTo(controller, [park], machine.z.feed, "raise Z off the switch");
    await moveXY(controller, machine, x0, y0, "return from the switch");
    return contact;
}

/** Returns the clear height the engaged Z was moved to, wire steps. */
export async function prepareZ(
    controller: Controller,
    machine: MachineConfig,
    head: number,
    profile: ToolProfile,
    materialMm: number,
    opts: PrepareZOptions = {},
): Promise<number> {
    const z = machine.heads[head]!.z;
    const heights = toolHeights(profile, machine, materialMm);
    const link = controller.link;

    let stored = (await getState(link)).probeZ;
    if (stored === undefined) {
        throw new Error("getstate carries no probed= field — the Pico firmware predates setprobe");
    }

    let touchedAt: readonly [number, number] | null = null;
    if (stored === null && z.probe !== undefined) {
        stored = await probeOnSwitch(controller, machine, head, profile, opts);
    } else if (stored === null) {
        if (opts.touchOff === undefined) {
            throw new Error(`head ${head}: Z has no probe config and no touch-off handler`);
        }
        const [x0, y0] = await getPos(link);
        opts.onLog?.(`head ${head} (${profile.name}): touch the tip off on the mat`);
        await opts.touchOff(head, profile);
        stored = (await getState(link)).probeZ ?? null;
        if (stored === null) throw new Error(`head ${head}: touch-off ended but the Pico holds no height`);
        opts.onLog?.(`head ${head} mat at Z ${stored} steps`);
        touchedAt = [x0, y0];
    }

    const clear = zAtHeightSteps(stored, heights.clearMm, z);
    await moveTo(controller, [zTarget(z, clear)], machine.z.feed, "move Z to clear height");
    if (touchedAt !== null) {
        await moveXY(controller, machine, touchedAt[0], touchedAt[1], "return from the touch-off");
    }
    return clear;
}
