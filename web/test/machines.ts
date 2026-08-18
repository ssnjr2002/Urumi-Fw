/**
 * test/machines.ts — hardcoded machines for tests. NOT the production path.
 *
 * Everything here bakes in calibration (stepsPerUnit, node ids, invert) that a
 * real machine must state in its config.json. Loading a real machine goes
 * through machine/json/load.ts; if production code imports this file, a
 * miscalibrated machine will run silently and cut wrong.
 *
 * That is exactly why these moved out of schema.ts: `pipelineConfig()` used to
 * default `machine` to defaultMachine(), so anyone calling `defaultConfig()` in
 * a production path got the 160/1200/51.667 test machine with no error. The
 * loader no longer touches this file at all — it builds its PipelineConfig
 * directly — so that path is now impossible to take by accident.
 *
 * It sits under test/ rather than src/ for the same reason, made structural:
 * a file outside the shipped tree cannot be imported by a shipped path at all,
 * and it no longer lands in dist/ for a consumer to find.
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
} from "../src/machine/schema.js";
import { KNIFE, TOOL_PROFILES } from "../src/machine/tools.js";
import { DEFAULTS } from "../src/machine/defaults.js";

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
        // maxAccel 300 mm/s^2 is PROVISIONAL — see docs/planner_audit.md H3.
        // Not measured on the bench yet; chosen as the smallest round value
        // that lets a 2 mm lift at 10 mm/s actually reach cruise (the ramp
        // needs v^2/2a = 0.17 mm per side, so 2d = 0.33 mm of a 2 mm move) and
        // that is ~3% of g in torque terms, which for a vertical leadscrew is
        // a rounding error on top of the static hold the motor already carries.
        // Replace it with a measured number: drive N up/down cycles and read
        // CMD_GET_POS for drift, bisecting on accel.
        axisConfig(busNode(3), 1200.0, { maxFeed: 10.0, maxAccel: 300.0, invert: true }),
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
