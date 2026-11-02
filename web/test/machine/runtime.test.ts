import { readFileSync } from "node:fs";
import { describe, expect, it } from "vitest";
import { DEFAULTS } from "../../src/machine/defaults.js";
import { f32, fitsF32 } from "../../src/machine/f32.js";
import { loadConfig } from "../../src/machine/json/load.js";
import {
    RUNTIME_FIELDS,
    runtimeFieldError,
    runtimeValues,
    type RuntimeField,
} from "../../src/machine/runtime.js";
import { renderRuntimeHeader } from "../../src/machine/runtimeHeader.js";
import { TOOL_PROFILES } from "../../src/machine/tools.js";

const field = (path: string): RuntimeField => {
    const f = RUNTIME_FIELDS.find((r) => r.path === path);
    if (!f) throw new Error(`no row ${path}`);
    return f;
};

function controller() {
    const axis = (id: number) => ({ node: { id }, stepsPerUnit: 160, maxTravel: 100 });
    const r = loadConfig(JSON.stringify({
        machine: {
            meshOn: false,
            laser: { node: 1 },
            x: { ...axis(1), homing: { kind: "linear", seekFeed: 10, latchFeed: 1, rampSteps: 0 } },
            y: axis(2),
            heads: [{
                accepts: ["knife"],
                z: axis(3),
                a: {
                    node: { id: 4 },
                    stepsPerUnit: 45,
                    rotary: true,
                    homing: { kind: "rotary", budgetRevs: 4, sweepFeed: 50, rampSteps: 0, toleranceDeg: 2 },
                },
            }],
        },
        tools: { knife: { id: 1 } },
    }));
    if (!r.ok) throw new Error(r.errors.join("\n"));
    return r.config;
}

describe("f32", () => {
    it("gives the shortest decimal with the same float32", () => {
        expect(f32(0.10000000149011612)).toBe(0.1);
        expect(f32(45.9847)).toBe(45.9847);
    });

    it("says whether the Pico keeps a value exactly", () => {
        expect(fitsF32(0.1)).toBe(true);
        expect(fitsF32(1200)).toBe(true);
        expect(fitsF32(1 / 3)).toBe(false);
    });
});

describe("the field table", () => {
    it("the committed header is current", () => {
        const url = new URL("../../../src/rp2350/core0/config/runtime_fields.h", import.meta.url);
        const committed = readFileSync(url, "utf8").replace(/\r\n/g, "\n");
        expect(committed).toBe(renderRuntimeHeader());
    });

    it("has one row per scope and path", () => {
        const keys = RUNTIME_FIELDS.map((f) => `${f.scope}:${f.path}`);
        expect(new Set(keys).size).toBe(keys.length);
    });

    it("every table field in the defaults and presets fits a float32", () => {
        const numbers = (v: unknown): number[] =>
            typeof v === "number" ? [v]
                : typeof v === "object" && v !== null ? Object.values(v).flatMap(numbers)
                    : [];
        const config = controller();
        const tools = Object.fromEntries(
            Object.entries(TOOL_PROFILES).map(([name, t]) => [name, { ...t, id: 1 }]),
        );
        const values = runtimeValues({ ...config, toolProfiles: tools });
        for (const { path, value } of values) {
            for (const n of numbers(value)) expect(fitsF32(n), path).toBe(true);
        }
        for (const n of numbers(DEFAULTS.machine.feed)) expect(fitsF32(n)).toBe(true);
        for (const n of numbers(DEFAULTS.machine.buffer)) expect(fitsF32(n)).toBe(true);
    });
});

describe("runtimeValues", () => {
    const paths = () => runtimeValues(controller()).map((v) => v.path);

    it("gives each concrete field its blob path", () => {
        expect(paths()).toContain("machine.feed.cutFallback");
        expect(paths()).toContain("machine.heads.0.z.maxFeed");
        expect(paths()).toContain("toolProfiles.knife.cutFeed");
    });

    it("offers a homing row only on an axis of that kind", () => {
        expect(paths()).toContain("machine.x.homing.seekFeed");
        expect(paths()).not.toContain("machine.heads.0.a.homing.seekFeed");
        expect(paths()).toContain("machine.heads.0.a.homing.sweepFeed");
        expect(paths()).not.toContain("machine.x.homing.sweepFeed");
        expect(paths()).not.toContain("machine.y.homing.startFeed");
    });
});

describe("runtimeFieldError", () => {
    it("applies inclusive and exclusive bounds", () => {
        expect(runtimeFieldError(field("feed.cutFallback"), 1)).toBeNull();
        expect(runtimeFieldError(field("feed.cutFallback"), 0.5)).toBe("must be >= 1 (got 0.5)");
        expect(runtimeFieldError(field("buffer.queueTime"), 0)).toBe("must be > 0 (got 0)");
        expect(runtimeFieldError(field("cornerAngleDeg"), 181)).toBe("must be <= 180 (got 181)");
    });

    it("rejects more precision than a float32 keeps", () => {
        expect(runtimeFieldError(field("zClearance"), 1 / 3)).toMatch(/more precision/);
    });

    it("checks both coordinates of a point", () => {
        expect(runtimeFieldError(field("positions.park"), { x: 1, y: 2 })).toBeNull();
        expect(runtimeFieldError(field("positions.park"), { x: 1 })).toMatch(/finite number/);
    });

    it("wants a whole number for steps", () => {
        expect(runtimeFieldError(field("homing.rampSteps"), 400)).toBeNull();
        expect(runtimeFieldError(field("homing.rampSteps"), 1.5)).toMatch(/whole number/);
        expect(runtimeFieldError(field("homing.rampSteps"), 70000)).toMatch(/<= 65535/);
    });
});
