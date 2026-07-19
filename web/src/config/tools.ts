/**
 * tools.ts — the tool preset catalogue.
 *
 * DATA, not schema. These change when you buy or build a tool; the shapes they
 * fill live in schema.ts. Split out so schema.ts is types + factories only —
 * adding a tool should never touch the schema file.
 *
 * A preset is the BASE for a tool: config.json's `tools.<name>` block patches
 * it (see load.ts), it is never redefined from scratch. Keys of TOOL_PROFILES
 * are the names a config.json and an SVG layer may reference.
 */

import { ToolType, toolProfile, type ToolProfile } from "./schema.js";

export const OFFSET_TOLERANCE_MM = 0.05;

export const PEN: ToolProfile = toolProfile("pen", {
    toolType: ToolType.PEN,
    tangential: false,
});

export const KNIFE: ToolProfile = toolProfile("knife", {
    toolType: ToolType.KNIFE,
    tangential: true,
    offsetMm: 0,
    unwind: true,
    cornerAngleDeg: 20,
});

export const CREASE: ToolProfile = toolProfile("crease", {
    toolType: ToolType.CREASE,
    tangential: true,
    offsetMm: 0,
    unwind: false,
    cornerAngleDeg: 30,
});

/**
 * Revolver pen: a rotating module with 7 slots for pens. The A axis
 * selects which slot is active (lowered). Each slot has a defined A
 * offset angle (360/7 ≈ 51.43° intervals). The active pen tip is at a
 * fixed XY offset from the head center regardless of which slot is
 * active — set via toolOffset.
 *
 * Not tangential — the A axis is used for slot selection, not tangent
 * tracking. The orchestrator jogs A to slotOffsets[i] before cutting
 * with slot i.
 *
 * slotOffsets: 7 angles at 360/7 intervals, starting at 0°.
 */
const REVOLVER_SLOT_COUNT = 7;
const REVOLVER_SLOT_INTERVAL = 360 / REVOLVER_SLOT_COUNT;
export const REVOLVER_PEN: ToolProfile = toolProfile("revolver_pen", {
    toolType: ToolType.REVOLVER_PEN,
    tangential: false,
    cornerAngleDeg: 30,
    slotOffsets: Array.from(
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

/**
 * True if the tool's blade offset is large enough to require (unimplemented)
 * offset compensation. The discretize stage refuses to run with a tool that
 * needs offset comp — it would cut wrong silently. Raise OFFSET_TOLERANCE_MM
 * only once compensation exists.
 */
export function needsOffsetComp(profile: ToolProfile): boolean {
    return profile.offsetMm > OFFSET_TOLERANCE_MM;
}
