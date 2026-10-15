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
import { DEFAULTS } from "../../src/machine/defaults.js";
import {
    axisConfig,
    busNode,
    machineConfig,
    qualityConfig,
    toolHead,
    toolProfile,
} from "../../src/machine/index.js";
import { parseConfig } from "../../src/machine/json/load.js";
import { resolveTargets } from "../../src/machine/resolve.js";

const axis = () => axisConfig(busNode(1), 160);
const head = () => toolHead(axisConfig(busNode(3), 300), axisConfig(busNode(4), 45));

describe("factories derive from DEFAULTS", () => {
    it("machineConfig uses DEFAULTS.machine", () => {
        const m = machineConfig(axis(), axis(), [head()]);
        expect(m.fCpu).toBe(DEFAULTS.machine.fCpu);
        expect(m.feed).toEqual(DEFAULTS.machine.feed);
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
        expect(t.plunge).toBe(DEFAULTS.tool.plunge);
        expect(t.tangential).toBe(DEFAULTS.tool.tangential);
    });

    it("overrides still beat the defaults", () => {
        const m = machineConfig(axis(), axis(), [head()], {
            feed: { cutFallback: 12345, travelDefault: 80, plungeFallback: 20, liftFeed: 20 },
        });
        expect(m.feed.cutFallback).toBe(12345);
    });
});

// ── fill-at-load ─────────────────────────────────────────────────────────────

/** Minimal valid config: only the required fields, every optional omitted. */
const MINIMAL = JSON.stringify({
    machine: {
        fCpu: 150_000_000,
        meshOn: false,
        x: { node: { id: 1 }, stepsPerUnit: 160 },
        y: { node: { id: 2 }, stepsPerUnit: 160 },
    },
    heads: [
        {
            accepts: ["knife"],
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
        expect(machine.feed).toEqual(DEFAULTS.machine.feed);
    });

    it("omitting an optional field equals stating its default explicitly", () => {
        const explicit = JSON.stringify({
            machine: {
                fCpu: 150_000_000,
                meshOn: false,
                feed: DEFAULTS.machine.feed,
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
                    accepts: ["knife"],
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
        feed: { cutFallback: 80, travelDefault: 70, plungeFallback: 20, liftFeed: 20 },
    });

    it("tool feeds override the machine fallbacks", () => {
        const t = resolveTargets(m, toolProfile("k", { cutFeed: 40, plungeFeed: 5 }));
        expect(t.cutFeed).toBe(40);
        expect(t.plungeFeed).toBe(5);
    });

    it("absent tool feeds inherit the machine fallbacks", () => {
        const t = resolveTargets(m, toolProfile("k"));
        expect(t.cutFeed).toBe(80);
        expect(t.plungeFeed).toBe(20);
    });

    it("travel and slew ignore tool overrides (machine-owned)", () => {
        const t = resolveTargets(m, toolProfile("k", { cutFeed: 40 }));
        expect(t.travelFeed).toBe(70);
    });
});
