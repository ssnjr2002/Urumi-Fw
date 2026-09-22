/**
 * Tests for the mount scheduler — fill/execute/pause/swap batching, and the
 * head assignment that falls out of it.
 *
 * The load-bearing cases are the two that are easy to get subtly wrong and
 * silently ship: filling by socket OCCUPANCY rather than distinct-tool count,
 * and placing MOST-CONSTRAINED-FIRST so the same tool set schedules the same
 * way whichever layer the SVG lists first.
 */

import { describe, it, expect } from "vitest";
import {
    scheduleMounts,
    mountDiff,
    validateMounts,
    type Mounts,
} from "../../src/production/schedule.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    toolHead,
    ToolType,
    type MachineConfig,
} from "../../src/machine/index.js";
import { twoHeadMachine } from "../machines.js";

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

const { PEN, KNIFE, CREASE, REVOLVER_PEN } = ToolType;
const empty = (n: number): Mounts => Array(n).fill(null);

describe("mountDiff", () => {
    it("reports tools added and removed", () => {
        const { added, removed } = mountDiff([PEN, KNIFE], [KNIFE, CREASE]);
        expect(added).toEqual([CREASE]);
        expect(removed).toEqual([PEN]);
    });

    it("empty diff for identical tables", () => {
        expect(mountDiff([PEN], [PEN])).toEqual({ added: [], removed: [] });
    });

    // The operator moves tools, not sockets. Swapping two tools between heads
    // is a real change to the machine but not to what is fitted, and the
    // prompts are about what to fit.
    it("ignores which socket a tool sits in", () => {
        expect(mountDiff([PEN, KNIFE], [KNIFE, PEN])).toEqual({ added: [], removed: [] });
    });

    it("treats an empty socket as nothing, not as a tool", () => {
        expect(mountDiff([PEN, null], [PEN, KNIFE])).toEqual({ added: [KNIFE], removed: [] });
    });
});

describe("validateMounts", () => {
    it("rejects a table of the wrong length", () => {
        expect(() => validateMounts(heads([KNIFE], [PEN]), [KNIFE])).toThrow(/2 head/);
    });

    it("rejects a tool in a socket that does not accept it", () => {
        expect(() => validateMounts(heads([KNIFE], [PEN]), [PEN, KNIFE]))
            .toThrow(/head 0 does not accept 'pen'/);
    });

    it("accepts empty sockets anywhere", () => {
        expect(() => validateMounts(heads([KNIFE], [PEN]), [null, null])).not.toThrow();
    });
});

describe("scheduleMounts", () => {
    it("no blocks → no phases", () => {
        expect(scheduleMounts(heads([KNIFE]), [], [null])).toEqual([]);
    });

    it("refuses a job needing a tool no head accepts", () => {
        expect(() => scheduleMounts(heads([KNIFE]), [PEN], [null]))
            .toThrow(/no head accepts 'pen'/);
    });

    it("refuses a starting table this machine cannot hold", () => {
        expect(() => scheduleMounts(heads([KNIFE], [PEN]), [KNIFE], [PEN, null]))
            .toThrow(/does not accept/);
    });

    it("runs in one pass when the tools fit the sockets", () => {
        const m = heads([KNIFE], [PEN]);
        const p = scheduleMounts(m, [KNIFE, PEN, KNIFE], empty(2));
        expect(p).toHaveLength(1);
        expect(p[0]!.mounts).toEqual([KNIFE, PEN]);
        expect(p[0]!.blockIndices).toEqual([0, 1, 2]);
        expect(p[0]!.swapOut).toEqual([]);
    });

    it("assigns the head as an output — mounts is head-indexed", () => {
        // The whole point: a block's head is the index holding its tool, so
        // this table IS the assignment. Head 1 takes the knife here purely
        // because head 0's fixture does not.
        const p = scheduleMounts(heads([PEN], [KNIFE]), [KNIFE], empty(2));
        expect(p[0]!.mounts).toEqual([null, KNIFE]);
        expect(p[0]!.mounts.indexOf(KNIFE)).toBe(1);
    });

    it("splits into phases when the job needs more tools than sockets", () => {
        const m = heads([KNIFE, CREASE], [PEN, CREASE]);
        const p = scheduleMounts(m, [KNIFE, PEN, CREASE, KNIFE], empty(2));
        expect(p).toHaveLength(2);
        expect(p[0]!.blockIndices).toEqual([0, 1]);
        expect(p[1]!.blockIndices).toEqual([2, 3]);
        expect(p[1]!.swapIn).toEqual([CREASE]);
        expect(p[1]!.swapOut).toEqual([PEN]);
    });

    it("keeps every block, in order, across all phases", () => {
        const m = heads([KNIFE, CREASE], [PEN, CREASE]);
        const p = scheduleMounts(m, [KNIFE, PEN, CREASE, KNIFE, PEN], empty(2));
        expect(p.flatMap((x) => x.blockIndices)).toEqual([0, 1, 2, 3, 4]);
    });

    it("one socket → a phase per tool change", () => {
        const p = scheduleMounts(heads([KNIFE, PEN]), [KNIFE, PEN, KNIFE], empty(1));
        expect(p.map((x) => x.mounts)).toEqual([[KNIFE], [PEN], [KNIFE]]);
        expect(p.map((x) => x.blockIndices)).toEqual([[0], [1], [2]]);
    });

    it("revolver slots share one head — no swap between slots", () => {
        // Blocks differ only by `slot` and share a toolType, so the scheduler
        // sees one tool. Slot changes are intra-phase A rotations.
        const p = scheduleMounts(
            heads([REVOLVER_PEN]),
            [REVOLVER_PEN, REVOLVER_PEN, REVOLVER_PEN],
            empty(1),
        );
        expect(p).toHaveLength(1);
        expect(p[0]!.blockIndices).toEqual([0, 1, 2]);
    });

    // Counting DISTINCT TOOLS would seat A and C together and return [A, C] —
    // both wanting head 0, a table no machine can hold. Counting occupancy
    // seats what fits, runs it, and swaps once.
    it("fills by socket occupancy, not distinct-tool count", () => {
        const m = heads([KNIFE, CREASE], [PEN]);
        const p = scheduleMounts(m, [KNIFE, PEN, CREASE], empty(2));
        expect(p[0]!.mounts).toEqual([KNIFE, PEN]);
        expect(p[0]!.blockIndices).toEqual([0, 1]);
        expect(p[1]!.mounts).toEqual([CREASE, null]);
    });

    // Blocks never reorder, so a permuted tool list must still produce the
    // same ARRANGEMENT. Filling in document order would give the crease head 0
    // in the second case, leaving the knife — which fits nowhere else — to open
    // a phase of its own.
    it("places most-constrained-first, so tool order does not change the result", () => {
        const m = heads([KNIFE, CREASE], [PEN, CREASE]);
        const a = scheduleMounts(m, [KNIFE, CREASE], empty(2));
        const b = scheduleMounts(m, [CREASE, KNIFE], empty(2));
        expect(a).toHaveLength(1);
        expect(b).toHaveLength(1);
        expect(a[0]!.mounts).toEqual([KNIFE, CREASE]);
        expect(b[0]!.mounts).toEqual([KNIFE, CREASE]);
    });

    describe("the mounts argument", () => {
        it("leaves a tool already correctly seated alone", () => {
            const m = heads([KNIFE, CREASE], [PEN, CREASE]);
            const p = scheduleMounts(m, [KNIFE, PEN], [KNIFE, PEN]);
            expect(p[0]!.swapIn).toEqual([]);
            expect(p[0]!.swapOut).toEqual([]);
        });

        // The operator moves tools, not sockets: a job wanting [B, A] when the
        // machine holds [A, B] has everything it needs already.
        it("does not swap merely because the sockets are the other way round", () => {
            const m = heads([KNIFE, CREASE], [KNIFE, CREASE]);
            const p = scheduleMounts(m, [CREASE, KNIFE], [KNIFE, CREASE]);
            expect(p).toHaveLength(1);
            expect(p[0]!.swapIn).toEqual([]);
            expect(p[0]!.swapOut).toEqual([]);
        });

        it("reports what must come out of a socket the job needs", () => {
            const p = scheduleMounts(heads([KNIFE, PEN]), [KNIFE], [PEN]);
            expect(p[0]!.swapIn).toEqual([KNIFE]);
            expect(p[0]!.swapOut).toEqual([PEN]);
        });

        // Reproducible vs swap-minimal is a caller choice, not an algorithm
        // choice — same function, different argument.
        it("a bare table gives the same schedule every time, whatever is fitted", () => {
            const m = heads([KNIFE, CREASE], [PEN, CREASE]);
            const bare = scheduleMounts(m, [KNIFE, PEN], empty(2));
            expect(scheduleMounts(m, [KNIFE, PEN], empty(2))).toEqual(bare);
            expect(scheduleMounts(m, [KNIFE, PEN], [CREASE, PEN])).not.toEqual(bare);
        });
    });

    it("works on the shared two-head fixture", () => {
        const m = twoHeadMachine();
        const p = scheduleMounts(m, [ToolType.KNIFE, ToolType.PEN], empty(2));
        expect(p).toHaveLength(1);
        expect(p[0]!.mounts).toEqual([ToolType.KNIFE, ToolType.PEN]);
    });
});
