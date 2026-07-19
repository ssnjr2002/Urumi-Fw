/**
 * defaults.test.ts — DEFAULTS is the single source of default values.
 *
 * Guards the drift class that motivated defaults.ts: the cut-feed default once
 * existed in four places (machineConfig, the loader fallback, and `?? 80` tails
 * in compileBlock/discretize/walk). These assert that the factories and the
 * loader both derive from DEFAULTS rather than restating it, so changing a
 * number here cannot leave a stale copy behind.
 */

import { describe, it, expect } from "vitest";
import { DEFAULTS } from "../../src/config/defaults.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    qualityConfig,
    toolHead,
    toolProfile,
} from "../../src/config/config.js";
import { parseConfig } from "../../src/config/load.js";
import { resolveTargets } from "../../src/config/resolve.js";

const axis = () => axisConfig(busNode(1), 160);
const head = () => toolHead(axisConfig(busNode(3), 300), axisConfig(busNode(4), 45));

describe("factories derive from DEFAULTS", () => {
    it("machineConfig uses DEFAULTS.machine", () => {
        const m = machineConfig(axis(), axis(), [head()]);
        expect(m.fCpu).toBe(DEFAULTS.machine.fCpu);
        expect(m.path).toEqual(DEFAULTS.machine.path);
        expect(m.rapid).toEqual(DEFAULTS.machine.rapid);
        expect(m.z).toEqual(DEFAULTS.machine.z);
        expect(m.slew).toEqual(DEFAULTS.machine.slew);
        expect(m.defaultHead).toBe(DEFAULTS.machine.defaultHead);
    });

    it("axisConfig uses DEFAULTS.axis", () => {
        const a = axisConfig(busNode(1), 160);
        expect(a.maxFeed).toBe(DEFAULTS.axis.maxFeed);
        expect(a.maxAccel).toBe(DEFAULTS.axis.maxAccel);
        expect(a.invert).toBe(DEFAULTS.axis.invert);
        expect(a.rotary).toBe(DEFAULTS.axis.rotary);
    });

    it("qualityConfig uses DEFAULTS.quality", () => {
        expect(qualityConfig()).toEqual(DEFAULTS.quality);
    });

    it("toolProfile uses DEFAULTS.tool", () => {
        const t = toolProfile("x");
        expect(t.cornerAngleDeg).toBe(DEFAULTS.tool.cornerAngleDeg);
        expect(t.liftHeight).toBe(DEFAULTS.tool.liftHeight);
        expect(t.tangential).toBe(DEFAULTS.tool.tangential);
    });

    it("overrides still beat the defaults", () => {
        const m = machineConfig(axis(), axis(), [head()], { path: { feed: 12345 } });
        expect(m.path.feed).toBe(12345);
    });
});

// ── fill-at-load ─────────────────────────────────────────────────────────────

/** Minimal valid config: only the required fields, every optional omitted. */
const MINIMAL = JSON.stringify({
    machine: {
        fCpu: 150_000_000,
        x: { node: { id: 1 }, stepsPerUnit: 160 },
        y: { node: { id: 2 }, stepsPerUnit: 160 },
    },
    heads: [
        {
            tool: "knife",
            z: { node: { id: 3 }, stepsPerUnit: 300 },
            a: { node: { id: 4 }, stepsPerUnit: 45 },
        },
    ],
});

describe("fill-at-load", () => {
    it("omitted machine targets are filled from DEFAULTS", () => {
        const r = parseConfig(MINIMAL);
        expect(r.ok).toBe(true);
        if (!r.ok) return;
        const { machine } = r.config;
        expect(machine.path).toEqual(DEFAULTS.machine.path);
        expect(machine.rapid).toEqual(DEFAULTS.machine.rapid);
        expect(machine.z).toEqual(DEFAULTS.machine.z);
    });

    it("omitting an optional field equals stating its default explicitly", () => {
        const explicit = JSON.stringify({
            machine: {
                fCpu: 150_000_000,
                path: DEFAULTS.machine.path,
                rapid: DEFAULTS.machine.rapid,
                z: DEFAULTS.machine.z,
                slew: DEFAULTS.machine.slew,
                x: {
                    node: { id: 1, type: 1, present: true },
                    stepsPerUnit: 160,
                    ...DEFAULTS.axis,
                },
                y: {
                    node: { id: 2, type: 1, present: true },
                    stepsPerUnit: 160,
                    ...DEFAULTS.axis,
                },
            },
            heads: [
                {
                    tool: "knife",
                    xOffset: 0,
                    yOffset: 0,
                    z: { node: { id: 3 }, stepsPerUnit: 300, ...DEFAULTS.axis },
                    a: { node: { id: 4 }, stepsPerUnit: 45, ...DEFAULTS.axis },
                },
            ],
            defaultHead: DEFAULTS.machine.defaultHead,
            peripherals: [],
            quality: DEFAULTS.quality,
        });

        const a = parseConfig(MINIMAL);
        const b = parseConfig(explicit);
        expect(a.ok && b.ok).toBe(true);
        if (!a.ok || !b.ok) return;
        expect(a.config).toEqual(b.config);
    });
});

// ── the override chain ───────────────────────────────────────────────────────

describe("resolveTargets", () => {
    const m = machineConfig(axis(), axis(), [head()], {
        path: { feed: 80 },
        z: { feed: 20 },
        rapid: { feed: 80 },
    });

    it("tool target overrides the machine baseline", () => {
        const t = resolveTargets(m, toolProfile("k", { path: { feed: 40 } }));
        expect(t.path.feed).toBe(40);
    });

    it("absent tool target inherits the machine baseline", () => {
        const t = resolveTargets(m, toolProfile("k"));
        expect(t.path.feed).toBe(80);
        expect(t.z.feed).toBe(20);
    });

    it("feed and accel inherit independently", () => {
        const machine = machineConfig(axis(), axis(), [head()], {
            path: { feed: 80, accel: 500 },
        });
        const t = resolveTargets(machine, toolProfile("k", { path: { feed: 40 } }));
        expect(t.path).toEqual({ feed: 40, accel: 500 });
    });

    it("rapid and slew ignore tool overrides (machine-owned)", () => {
        const t = resolveTargets(m, toolProfile("k", { path: { feed: 40 } }));
        expect(t.rapid.feed).toBe(80);
    });
});
