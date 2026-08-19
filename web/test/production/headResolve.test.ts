/**
 * headResolve.test.ts — the SPEC for docs/head_binding.md.
 *
 * The contract: a block's tool runs on a head whose fixture accepts it
 * (machine.heads[].accepts), and the block's Z/A motion must be discretized
 * against THAT head's calibration, not `machine.defaultHead`'s. Nothing here
 * asserts on HOW that gets decided — it only inspects the segments a public
 * bake produces, so it is satisfied equally by the design that landed
 * (schedule, then compile against the chosen head) or by any other. This was
 * the acceptance test for docs/head_binding.md, written red and now green.
 *
 * Mechanism: two profiles sharing a liftHeight but bound to heads with
 * different Z calibration (1200 vs 600 steps/mm, twoHeadMachine's ratio — see
 * its doc comment in test/machines.ts). A lift of the same physical height
 * must produce a DIFFERENT step count on each head; a test built on identical
 * heads could not tell a correct resolve from a wrong one.
 */

import { describe, it, expect } from "vitest";
import { bakePlan } from "../../src/production/bakePlan.js";
import { toolProfile, ToolType, type PipelineConfig } from "../../src/machine/index.js";
import { qualityConfig } from "../../src/machine/schema.js";
import { MICRO_LIFT } from "../../src/wire/format/microsegment.js";
import { twoHeadMachine } from "../machines.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    type MachineConfig,
} from "../../src/machine/index.js";

/** A machine whose heads accept exactly what each argument says. */
function heads(...accepts: ToolType[][]): MachineConfig {
    return machineConfig(
        axisConfig(busNode(1), 160),
        axisConfig(busNode(2), 160),
        accepts.map((a, i) =>
            toolHead(
                axisConfig(busNode(3 + i * 2), 1200),
                axisConfig(busNode(4 + i * 2), 51.667, { rotary: true }),
                { accepts: a },
            ),
        ),
    );
}

const wrap = (inner: string) =>
    `<svg xmlns="http://www.w3.org/2000/svg" xmlns:inkscape="http://www.inkscape.org/namespaces/inkscape" ` +
    `width="100mm" height="100mm" viewBox="0 0 100 100">${inner}</svg>`;
const tri = (x: number, y: number) =>
    `<path d="M${x},${y} L${x + 10},${y} L${x + 10},${y + 10} Z"/>`;

const LIFT_MM = 2;

/** knife → head 0 (1200 spu), pen → head 1 (600 spu). Same lift height. */
function config(): PipelineConfig {
    return {
        machine: twoHeadMachine(),
        quality: qualityConfig(),
        toolProfiles: {
            knife: toolProfile("knife", { toolType: ToolType.KNIFE, liftHeight: LIFT_MM }),
            pen: toolProfile("pen", { toolType: ToolType.PEN, liftHeight: LIFT_MM }),
        },
    };
}

/** Total |dz| across every MICRO_LIFT segment — the round-trip step count for one lift+lower. */
function liftSteps(segments: readonly { flags: number; dz: number }[]): number {
    return segments.filter((s) => s.flags & MICRO_LIFT).reduce((sum, s) => sum + Math.abs(s.dz), 0);
}

describe("head resolution — spec", () => {
    it("resolves each block's Z against the head that actually holds its tool", () => {
        const { blocks } = bakePlan(config(), wrap(`<g id="knife">${tri(10, 10)}</g><g id="pen">${tri(30, 30)}</g>`));
        const knife = blocks.find((b) => b.profile.name === "knife")!;
        const pen = blocks.find((b) => b.profile.name === "pen")!;

        // Same physical lift, different heads, different calibration — the
        // step counts MUST differ. 1200 vs 600 steps/mm on a 2mm lift, counted
        // over the round trip (liftSteps sums |dz|, so one lift plus its lower).
        expect(liftSteps(knife.segments)).toBe(2 * Math.round(LIFT_MM * 1200));
        expect(liftSteps(pen.segments)).toBe(2 * Math.round(LIFT_MM * 600));

        // And the choice is recorded, not left to be re-derived downstream.
        expect(knife.head).toBe(0);
        expect(pen.head).toBe(1);
    });

    it("the two heads disagree — which is what makes the assertion above mean anything", () => {
        // A fixture whose heads shared one calibration would pass whether or
        // not the resolve was correct. Pin the thing that gives the test teeth.
        const m = twoHeadMachine();
        expect(m.heads[0]!.z.stepsPerUnit).not.toBe(m.heads[1]!.z.stepsPerUnit);
    });

    it("a job using a tool no head declares is refused at bake, not at the machine", () => {
        const noHome = toolProfile("crease", { toolType: ToolType.CREASE, liftHeight: LIFT_MM });
        const cfg: PipelineConfig = {
            machine: heads([ToolType.KNIFE], [ToolType.PEN]), // neither accepts crease
            quality: qualityConfig(),
            toolProfiles: { crease: noHome },
        };
        expect(() =>
            bakePlan(cfg, wrap(`<g id="crease">${tri(10, 10)}</g>`)),
        ).toThrow(/no head/i);
    });
});
