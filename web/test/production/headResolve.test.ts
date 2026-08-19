/**
 * headResolve.test.ts — the SPEC for docs/head_binding.md.
 *
 * The contract: a block's tool has ONE home head (machine.heads[].profile),
 * and the block's Z/A motion must be discretized against THAT head's
 * calibration, not `machine.defaultHead`'s. Nothing here asserts on HOW that
 * gets decided — no `Block.head` field, no particular function signature. It
 * only inspects the segments a public bake produces, so it is satisfied
 * equally by resolving the axes eagerly inside compileBlock, by attaching a
 * head index to Block and resolving later, or by a future design where a
 * block's compute is deferred until a head is chosen for it. Whichever
 * mechanism lands, this file is the acceptance test — unskipping the second
 * describe block is the definition of done, mirroring dutyInsert.test.ts.
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

describe("head resolution — current behaviour (pinned, not the spec)", () => {
    it("BUG: both tools resolve against defaultHead, so a 2mm lift costs the same steps either way", () => {
        // This is what docs/head_binding.md exists to fix. Pinned so the suite
        // tells us the moment it stops being true — at which point this test
        // starts failing and should be deleted, not "fixed".
        const { plan } = bakePlan(config(), wrap(`<g id="knife">${tri(10, 10)}</g><g id="pen">${tri(30, 30)}</g>`));
        const knifeSteps = liftSteps(plan.blocks[0]!.segments);
        const penSteps = liftSteps(plan.blocks[1]!.segments);
        expect(knifeSteps).toBeGreaterThan(0);
        // Wrong: pen is on head 1 (600 spu) but comes out identical to knife's
        // head-0 (1200 spu) resolve. Two different physical heads producing
        // the same step count for the same mm of travel is the bug itself.
        expect(penSteps).toBe(knifeSteps);
    });
});

describe.skip("head resolution — spec", () => {
    it("resolves each block's Z against the head that actually holds its tool", () => {
        const { plan } = bakePlan(config(), wrap(`<g id="knife">${tri(10, 10)}</g><g id="pen">${tri(30, 30)}</g>`));
        const knife = plan.blocks.find((b) => b.profile.name === "knife")!;
        const pen = plan.blocks.find((b) => b.profile.name === "pen")!;

        // Same physical lift, different heads, different calibration — the
        // step counts MUST differ. 1200 vs 600 steps/mm on a 2mm lift.
        expect(liftSteps(knife.segments)).toBe(Math.round(LIFT_MM * 1200));
        expect(liftSteps(pen.segments)).toBe(Math.round(LIFT_MM * 600));
    });

    it("a job using a tool no head declares is refused at bake, not at the machine", () => {
        const noHome = toolProfile("crease", { toolType: ToolType.CREASE, liftHeight: LIFT_MM });
        const cfg: PipelineConfig = {
            machine: twoHeadMachine(), // neither head seeds crease
            quality: qualityConfig(),
            toolProfiles: { crease: noHome },
        };
        expect(() =>
            bakePlan(cfg, wrap(`<g id="crease">${tri(10, 10)}</g>`)),
        ).toThrow(/no head/i);
    });
});
