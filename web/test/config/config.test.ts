/**
 * Tests for config.ts — fresh (no Python test_config.py to port).
 * Verifies defaultConfig calibration, uniformMachine builder, preset
 * values, registries, quality defaults, and pipelineConfig overrides.
 */

import { describe, it, expect } from "vitest";
import {
    OFFSET_TOLERANCE_MM,
    PEN,
    KNIFE,
    CREASE,
    REVOLVER_PEN,
    TOOL_PROFILES,
    TOOL_PROFILES_BY_TYPE,
    ToolType,
    busNode,
    axisConfig,
    toolProfile,
    toolHead,
    machineConfig,
    uniformMachine,
    qualityConfig,
    pipelineConfig,
    defaultConfig,
} from "../../src/config/config.js";

describe("config: BusNode", () => {
    it("defaults role=stepper, present=true", () => {
        const n = busNode(1);
        expect(n.nodeId).toBe(1);
        expect(n.role).toBe("stepper");
        expect(n.present).toBe(true);
    });

    it("accepts overrides", () => {
        const n = busNode(5, { role: "oscillator", present: false });
        expect(n.nodeId).toBe(5);
        expect(n.role).toBe("oscillator");
        expect(n.present).toBe(false);
    });
});

describe("config: AxisConfig", () => {
    it("defaults maxRate=0, accel=0, invert=false, rotary=false", () => {
        const a = axisConfig(busNode(1), 160.0);
        expect(a.stepsPerUnit).toBe(160.0);
        expect(a.maxRate).toBe(0);
        expect(a.accel).toBe(0);
        expect(a.invert).toBe(false);
        expect(a.rotary).toBe(false);
    });

    it("accepts overrides", () => {
        const a = axisConfig(busNode(4), 51.667, { rotary: true, invert: true, maxRate: 100, accel: 2000 });
        expect(a.rotary).toBe(true);
        expect(a.invert).toBe(true);
        expect(a.maxRate).toBe(100);
        expect(a.accel).toBe(2000);
    });
});

describe("config: ToolType", () => {
    it("has stable numeric values", () => {
        expect(ToolType.PEN).toBe(0x01);
        expect(ToolType.KNIFE).toBe(0x02);
        expect(ToolType.CREASE).toBe(0x03);
        expect(ToolType.REVOLVER_PEN).toBe(0x04);
    });
});

describe("config: ToolProfile presets", () => {
    it("PEN: non-tangential, type PEN", () => {
        expect(PEN.name).toBe("pen");
        expect(PEN.toolType).toBe(ToolType.PEN);
        expect(PEN.tangential).toBe(false);
        expect(PEN.feedMax).toBe(80);
    });

    it("KNIFE: tangential, wired (unwind), type KNIFE", () => {
        expect(KNIFE.name).toBe("knife");
        expect(KNIFE.toolType).toBe(ToolType.KNIFE);
        expect(KNIFE.tangential).toBe(true);
        expect(KNIFE.unwind).toBe(true);
        expect(KNIFE.offsetMm).toBe(0);
        expect(KNIFE.cornerAngleDeg).toBe(20);
    });

    it("CREASE: tangential, free-spinning (no unwind), type CREASE", () => {
        expect(CREASE.name).toBe("crease");
        expect(CREASE.toolType).toBe(ToolType.CREASE);
        expect(CREASE.tangential).toBe(true);
        expect(CREASE.unwind).toBe(false);
        expect(CREASE.cornerAngleDeg).toBe(30);
    });

    it("all presets have empty requiredPeripheralRoles", () => {
        expect(PEN.requiredPeripheralRoles).toEqual([]);
        expect(KNIFE.requiredPeripheralRoles).toEqual([]);
        expect(CREASE.requiredPeripheralRoles).toEqual([]);
        expect(REVOLVER_PEN.requiredPeripheralRoles).toEqual([]);
    });

    it("PEN/KNIFE/CREASE have default toolOffset (0,0) and no slotOffsets", () => {
        expect(PEN.toolOffset).toEqual({ xOffset: 0, yOffset: 0 });
        expect(KNIFE.toolOffset).toEqual({ xOffset: 0, yOffset: 0 });
        expect(CREASE.toolOffset).toEqual({ xOffset: 0, yOffset: 0 });
        expect(PEN.slotOffsets).toBeUndefined();
        expect(KNIFE.slotOffsets).toBeUndefined();
        expect(CREASE.slotOffsets).toBeUndefined();
    });
});

describe("config: REVOLVER_PEN preset", () => {
    it("is non-tangential, type REVOLVER_PEN", () => {
        expect(REVOLVER_PEN.name).toBe("revolver_pen");
        expect(REVOLVER_PEN.toolType).toBe(ToolType.REVOLVER_PEN);
        expect(REVOLVER_PEN.tangential).toBe(false);
    });

    it("has 7 slot offsets at 360/7 degree intervals", () => {
        expect(REVOLVER_PEN.slotOffsets).toHaveLength(7);
        expect(REVOLVER_PEN.slotOffsets![0]).toBe(0);
        expect(REVOLVER_PEN.slotOffsets![1]).toBeCloseTo(360 / 7, 5);
        expect(REVOLVER_PEN.slotOffsets![6]).toBeCloseTo(6 * 360 / 7, 5);
    });

    it("has a toolOffset (placeholder 0,0 until measured)", () => {
        expect(REVOLVER_PEN.toolOffset).toEqual({ xOffset: 0, yOffset: 0 });
    });
});

describe("config: TOOL_PROFILES registries", () => {
    it("TOOL_PROFILES keyed by name with 4 entries", () => {
        expect(Object.keys(TOOL_PROFILES)).toHaveLength(4);
        expect(TOOL_PROFILES.pen).toBe(PEN);
        expect(TOOL_PROFILES.knife).toBe(KNIFE);
        expect(TOOL_PROFILES.crease).toBe(CREASE);
        expect(TOOL_PROFILES.revolver_pen).toBe(REVOLVER_PEN);
    });

    it("TOOL_PROFILES_BY_TYPE keyed by toolType", () => {
        expect(TOOL_PROFILES_BY_TYPE[ToolType.PEN]).toBe(PEN);
        expect(TOOL_PROFILES_BY_TYPE[ToolType.KNIFE]).toBe(KNIFE);
        expect(TOOL_PROFILES_BY_TYPE[ToolType.CREASE]).toBe(CREASE);
        expect(TOOL_PROFILES_BY_TYPE[ToolType.REVOLVER_PEN]).toBe(REVOLVER_PEN);
    });
});

describe("config: ToolHead", () => {
    it("defaults profile=PEN, xOffset=0, yOffset=0", () => {
        const z = axisConfig(busNode(3), 1200);
        const a = axisConfig(busNode(4), 51.667, { rotary: true });
        const h = toolHead(z, a);
        expect(h.z).toBe(z);
        expect(h.a).toBe(a);
        expect(h.profile).toBe(PEN);
        expect(h.xOffset).toBe(0);
        expect(h.yOffset).toBe(0);
    });

    it("accepts profile and XY offset overrides", () => {
        const h = toolHead(
            axisConfig(busNode(3), 1200),
            axisConfig(busNode(4), 51.667, { rotary: true }),
            { profile: KNIFE, xOffset: 50, yOffset: -10 },
        );
        expect(h.profile).toBe(KNIFE);
        expect(h.xOffset).toBe(50);
        expect(h.yOffset).toBe(-10);
    });
});

describe("config: MachineConfig laser", () => {
    it("defaults to no laser", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160),
            [toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667, { rotary: true }))],
        );
        expect(m.laser).toBeUndefined();
    });

    it("accepts a laser pointer reference", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160),
            [
                toolHead(
                    axisConfig(busNode(3), 1200),
                    axisConfig(busNode(4), 51.667, { rotary: true }),
                    { xOffset: -50 },
                ),
            ],
            { laser: { xOffset: 0, yOffset: 0 } },
        );
        expect(m.laser).toEqual({ xOffset: 0, yOffset: 0 });
        expect(m.heads[0]!.xOffset).toBe(-50);
    });
});

describe("config: MachineConfig", () => {
    it("defaults defaultHead=0, fCpu=150e6, jogFeed=80, zFeed=20, peripherals=[]", () => {
        const m = machineConfig(
            axisConfig(busNode(1), 160),
            axisConfig(busNode(2), 160),
            [toolHead(axisConfig(busNode(3), 1200), axisConfig(busNode(4), 51.667, { rotary: true }))],
        );
        expect(m.defaultHead).toBe(0);
        expect(m.fCpu).toBe(150_000_000);
        expect(m.jogFeed).toBe(80);
        expect(m.zFeed).toBe(20);
        expect(m.peripherals).toEqual([]);
    });
});

describe("config: uniformMachine", () => {
    it("builds symmetric X/Y with equal stepsPerUnit", () => {
        const m = uniformMachine(160, 51.667);
        expect(m.x.stepsPerUnit).toBe(160);
        expect(m.y.stepsPerUnit).toBe(160);
        expect(m.x.node.nodeId).toBe(1);
        expect(m.y.node.nodeId).toBe(2);
    });

    it("head Z=node3, A=node4 rotary, default profile KNIFE", () => {
        const m = uniformMachine(160, 51.667);
        const head = m.heads[0]!;
        expect(head.z.node.nodeId).toBe(3);
        expect(head.a.node.nodeId).toBe(4);
        expect(head.a.rotary).toBe(true);
        expect(head.profile).toBe(KNIFE);
    });

    it("accepts maxRate/accel/profile options", () => {
        const m = uniformMachine(160, 51.667, { maxRate: 100, accel: 2000, profile: PEN });
        expect(m.x.maxRate).toBe(100);
        expect(m.x.accel).toBe(2000);
        expect(m.heads[0]!.profile).toBe(PEN);
    });
});

describe("config: defaultConfig", () => {
    const cfg = defaultConfig();

    it("X: 160 steps/mm, node 1, invert, maxRate 80, accel 1000", () => {
        expect(cfg.machine.x.stepsPerUnit).toBe(160);
        expect(cfg.machine.x.node.nodeId).toBe(1);
        expect(cfg.machine.x.invert).toBe(true);
        expect(cfg.machine.x.maxRate).toBe(80);
        expect(cfg.machine.x.accel).toBe(1000);
    });

    it("Y: 160 steps/mm, node 2, no invert, maxRate 80, accel 1000", () => {
        expect(cfg.machine.y.stepsPerUnit).toBe(160);
        expect(cfg.machine.y.node.nodeId).toBe(2);
        expect(cfg.machine.y.invert).toBe(false);
        expect(cfg.machine.y.maxRate).toBe(80);
        expect(cfg.machine.y.accel).toBe(1000);
    });

    it("Z: 1200 steps/mm, node 3, invert, maxRate 10", () => {
        const z = cfg.machine.heads[0]!.z;
        expect(z.stepsPerUnit).toBe(1200);
        expect(z.node.nodeId).toBe(3);
        expect(z.invert).toBe(true);
        expect(z.maxRate).toBe(10);
        expect(z.rotary).toBe(false);
    });

    it("A: 51.667 steps/deg, node 4, rotary, invert, maxRate 100, accel 2000", () => {
        const a = cfg.machine.heads[0]!.a;
        expect(a.stepsPerUnit).toBeCloseTo(51.667, 3);
        expect(a.node.nodeId).toBe(4);
        expect(a.rotary).toBe(true);
        expect(a.invert).toBe(true);
        expect(a.maxRate).toBe(100);
        expect(a.accel).toBe(2000);
    });

    it("single head with KNIFE profile, offset (0, 0)", () => {
        expect(cfg.machine.heads).toHaveLength(1);
        expect(cfg.machine.heads[0]!.profile).toBe(KNIFE);
        expect(cfg.machine.heads[0]!.xOffset).toBe(0);
        expect(cfg.machine.heads[0]!.yOffset).toBe(0);
    });

    it("machine defaults: defaultHead 0, fCpu 150e6, jogFeed 80, zFeed 20, no laser", () => {
        expect(cfg.machine.defaultHead).toBe(0);
        expect(cfg.machine.fCpu).toBe(150_000_000);
        expect(cfg.machine.jogFeed).toBe(80);
        expect(cfg.machine.zFeed).toBe(20);
        expect(cfg.machine.laser).toBeUndefined();
    });

    it("toolProfiles has 4 entries (pen, knife, crease, revolver_pen)", () => {
        expect(cfg.toolProfiles.pen).toBe(PEN);
        expect(cfg.toolProfiles.knife).toBe(KNIFE);
        expect(cfg.toolProfiles.crease).toBe(CREASE);
        expect(cfg.toolProfiles.revolver_pen).toBe(REVOLVER_PEN);
    });
});

describe("config: QualityConfig", () => {
    it("defaults match the parity spec", () => {
        const q = qualityConfig();
        expect(q.chordTol).toBe(0.01);
        expect(q.dvMax).toBe(3.0);
        expect(q.vMin).toBe(0.5);
        expect(q.dtMax).toBe(0.05);
        expect(q.dtMin).toBe(1e-6);
        expect(q.angleTol).toBe(5.0);
        expect(q.gapTol).toBe(0.01);
        expect(q.nKappa).toBe(20);
        expect(q.junctionDeviation).toBe(0.05);
        expect(q.dsMax).toBe(0.5);
        expect(q.dthetaMax).toBe(2.0);
    });

    it("accepts overrides", () => {
        const q = qualityConfig({ chordTol: 0.005, dsMax: 0.3 });
        expect(q.chordTol).toBe(0.005);
        expect(q.dsMax).toBe(0.3);
        expect(q.dvMax).toBe(3.0);
    });
});

describe("config: pipelineConfig overrides", () => {
    it("patches machine with a custom one", () => {
        const custom = uniformMachine(200, 100);
        const cfg = pipelineConfig({ machine: custom });
        expect(cfg.machine.x.stepsPerUnit).toBe(200);
        expect(cfg.machine.y.stepsPerUnit).toBe(200);
    });

    it("patches quality", () => {
        const cfg = pipelineConfig({ quality: qualityConfig({ vMin: 1.0 }) });
        expect(cfg.quality.vMin).toBe(1.0);
    });

    it("patches toolProfiles", () => {
        const fastPen = toolProfile("pen", { feedMax: 120 });
        const cfg = pipelineConfig({ toolProfiles: { pen: fastPen } });
        expect(cfg.toolProfiles.pen?.feedMax).toBe(120);
    });
});

describe("config: OFFSET_TOLERANCE_MM", () => {
    it("is 0.05", () => {
        expect(OFFSET_TOLERANCE_MM).toBe(0.05);
    });
});
