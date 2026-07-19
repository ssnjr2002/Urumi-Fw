/**
 * fixtures.ts — hardcoded machines for tests and demos. NOT the production path.
 *
 * Everything here bakes in calibration (stepsPerUnit, node ids, invert) that a
 * real machine must state in its config.json. Loading a real machine goes
 * through load.ts; if production code imports this file, a miscalibrated
 * machine will run silently and cut wrong.
 *
 * That is exactly why these moved out of schema.ts: `pipelineConfig()` used to
 * default `machine` to defaultMachine(), so anyone calling `defaultConfig()` in
 * a production path got the 160/1200/51.667 test machine with no error. The
 * loader no longer touches this file at all — it builds its PipelineConfig
 * directly — so that path is now impossible to take by accident.
 */

import {
    axisConfig,
    busNode,
    machineConfig,
    qualityConfig,
    toolHead,
    type MachineConfig,
    type PipelineConfig,
    type ToolProfile,
} from "./schema.js";
import { KNIFE, TOOL_PROFILES } from "./tools.js";
import { DEFAULTS } from "./defaults.js";

/**
 * Build an equal-XY (single belt/pulley) single-head machine using the
 * conventional X=node1, Y=node2, Z=node3, A=node4 map. Convenience for the
 * common simple case. The single head is centred (x_offset = 0) and carries
 * `profile` (default KNIFE).
 */
export function uniformMachine(
    stepsPerMm: number,
    stepsPerDeg: number,
    options?: {
        fCpu?: number;
        maxFeed?: number;
        maxAccel?: number;
        profile?: ToolProfile;
    },
): MachineConfig {
    const {
        fCpu = DEFAULTS.machine.fCpu,
        maxFeed = 80,
        maxAccel = 1000,
        profile = KNIFE,
    } = options ?? {};
    const head = toolHead(
        axisConfig(busNode(3), stepsPerMm, { maxFeed, maxAccel }),
        axisConfig(busNode(4), stepsPerDeg, { maxFeed, maxAccel, rotary: true }),
        { profile },
    );
    return machineConfig(
        axisConfig(busNode(1), stepsPerMm, { maxFeed, maxAccel }),
        axisConfig(busNode(2), stepsPerMm, { maxFeed, maxAccel }),
        [head],
        { fCpu },
    );
}

/**
 * The physical bench machine: DM542 @ 1/32 microstepping.
 *   X/Y : GT2 20T pulley, 40 mm/rev -> 160 steps/mm
 *   Z   : lead screw -> 1200 steps/mm
 *   A   : tangential rotary -> 51.667 steps/deg
 * Node map X=1, Y=2, Z=3, A=4. Single centred head, KNIFE mounted.
 */
export function defaultMachine(): MachineConfig {
    const head = toolHead(
        axisConfig(busNode(3), 1200.0, { maxFeed: 10.0, invert: true }),
        axisConfig(busNode(4), 51.667, {
            maxFeed: 100.0,
            maxAccel: 2000.0,
            invert: true,
            rotary: true,
        }),
        { profile: KNIFE },
    );
    return machineConfig(
        axisConfig(busNode(1), 160.0, { maxFeed: 80.0, maxAccel: 1000.0, invert: true }),
        axisConfig(busNode(2), 160.0, { maxFeed: 80.0, maxAccel: 1000.0 }),
        [head],
    );
}

/** A complete PipelineConfig around defaultMachine(). Tests/demos only. */
export function pipelineConfig(overrides?: Partial<PipelineConfig>): PipelineConfig {
    return {
        machine: defaultMachine(),
        quality: qualityConfig(),
        toolProfiles: { ...TOOL_PROFILES },
        ...overrides,
    };
}

/** Alias for pipelineConfig() with no overrides. Tests/demos only. */
export function defaultConfig(): PipelineConfig {
    return pipelineConfig();
}
