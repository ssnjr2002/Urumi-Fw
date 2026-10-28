/**
 * save.ts — PipelineConfig → a config file that loadConfig reads back to the
 * same model. Used to download a config pulled from the Pico.
 *
 * Filled-in defaults stay in the file. Three reshapes undo what load.ts does:
 *   - toolProfiles → tools
 *   - each tool loses name, toolType and requiredPeripheralTypes, which
 *     loading rederives from the preset
 *   - heads[].accepts goes from ToolType back to preset names
 */

import type { PipelineConfig } from "../schema.js";
import { TOOL_PROFILES_BY_TYPE } from "../tools.js";

export function modelToFile(config: PipelineConfig): string {
    const tools: Record<string, object> = {};
    for (const [key, profile] of Object.entries(config.toolProfiles)) {
        const { name: _n, toolType: _t, requiredPeripheralTypes: _r, ...rest } = profile;
        tools[key] = rest;
    }
    const heads = config.machine.heads.map((h) => ({
        ...h,
        accepts: h.accepts.map((t) => TOOL_PROFILES_BY_TYPE[t]!.name),
    }));
    return JSON.stringify(
        { machine: { ...config.machine, heads }, quality: config.quality, tools },
        null,
        2,
    );
}
