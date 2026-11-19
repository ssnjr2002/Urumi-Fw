/**
 * tools.ts — the tool presets: data, not schema. A config's `tools.<name>`
 * patches the preset of that name (load.ts). The keys are the names a config
 * and an SVG layer use.
 */

import { ToolType, toolProfile, type ToolProfile } from "./schema.js";

export const PEN: ToolProfile = toolProfile("pen", {
    toolType: ToolType.PEN,
    tangential: false,
});

export const KNIFE: ToolProfile = toolProfile("knife", {
    toolType: ToolType.KNIFE,
    tangential: true,
    plunge: true,
    offsetMm: 0,
    unwind: true,
    cornerAngleDeg: 20,
});

export const CREASE: ToolProfile = toolProfile("crease", {
    toolType: ToolType.CREASE,
    tangential: true,
    plunge: true,
    offsetMm: 0,
    unwind: false,
    periodCount: 2,
    cornerAngleDeg: 30,
});

/**
 * Revolver pen: a rotating module with 7 pens, selected by turning A to
 * chamberOffsets[i] (every 360/7°, from 0°). Not tangential. The active pen
 * tip sits at a fixed offset from the head centre (toolOffset).
 */
const REVOLVER_SLOT_COUNT = 7;
const REVOLVER_SLOT_INTERVAL = 360 / REVOLVER_SLOT_COUNT;
export const REVOLVER_PEN: ToolProfile = toolProfile("revolver_pen", {
    toolType: ToolType.REVOLVER_PEN,
    tangential: false,
    cornerAngleDeg: 30,
    chamberOffsets: Array.from(
        { length: REVOLVER_SLOT_COUNT },
        (_, i) => i * REVOLVER_SLOT_INTERVAL,
    ),
    // TODO: measure the real pen tip offset from head center
    toolOffset: { xOffset: 0, yOffset: 0 },
});

export const TOOL_PROFILES: Readonly<Record<string, ToolProfile>> = {
    pen: PEN,
    knife: KNIFE,
    crease: CREASE,
    revolver_pen: REVOLVER_PEN,
};

export const TOOL_PROFILES_BY_TYPE: Readonly<Record<number, ToolProfile>> = {
    [ToolType.PEN]: PEN,
    [ToolType.KNIFE]: KNIFE,
    [ToolType.CREASE]: CREASE,
    [ToolType.REVOLVER_PEN]: REVOLVER_PEN,
};
