/**
 * frames.ts — the machine's coordinate frames, as pure transforms.
 *
 * Three frames (docs/coordinate_frames_and_limits.md §1):
 *
 *   wire   per-slot int32 step counters. Invert applied. Firmware-owned.
 *   home   mm. The firmware's position ÷ stepsPerUnit. Datum from homing;
 *          `setorigin` overrides it. THE machine frame — the only one the
 *          firmware knows, and the only one the soft-limit envelope lives in.
 *   tool   home + head offset + tool offset. Host-side only, per selected
 *          head/tool. The "tip" position the operator reads.
 *
 * This module owns home↔tool, and wire↔home for the callers that need to show
 * or target a position in mm.
 *
 * Wire↔home sits here TEMPORARILY. It is per-axis calibration math, not frame
 * geometry, and `operatorJog` already does the forward half inline; the two
 * should end up together once there is an obvious home for them. Keeping them
 * in one file for now beats a third copy of the conversion.
 *
 * X/Y ONLY. X and Y are one degree of freedom each — gantry, anchor and every
 * tip are one rigid body differing by fixed offsets, so a position plus an
 * offset is the whole model (§2). Z and A are separate motors with their own
 * AxisConfig per head: selecting the head selects the axis outright, and no
 * offset arithmetic applies. Nothing here takes a Z or an A.
 *
 * Everything is a pure function over config. Choosing which tip to jog around
 * is a view transform, not state — no datum is written and nothing here has to
 * be re-asserted after a reconnect.
 */

import type { MachineConfig, ToolProfile } from "./schema.js";

/** A point or offset in the XY plane, in mm. */
export interface XY {
    readonly x: number;
    readonly y: number;
}

// ── the anchor ───────────────────────────────────────────────────────────────

/**
 * Which entity config places at (0,0). The laser when one is fitted, otherwise
 * the head mounted at the origin, and `none` when the config offsets every head
 * away from (0,0) — legal, but it means the origin is a bare point in space
 * with no hardware at it, which is worth surfacing rather than assuming.
 */
export type Anchor =
    | { readonly kind: "laser" }
    | { readonly kind: "head"; readonly index: number }
    | { readonly kind: "none" };

/**
 * Identify the anchor — the entity whose position IS the machine position,
 * since its offset is (0,0) by construction.
 *
 * DESCRIPTIVE ONLY: it does no arithmetic, because config offsets are already
 * expressed in one frame. Use it to label the origin in a UI and to let
 * validation assert that something is actually at it. The transforms below
 * never need it.
 *
 * Call the result "the anchor", never "the reference head" — with a laser
 * fitted, no head holds the origin.
 */
export function machineAnchor(machine: MachineConfig): Anchor {
    if (machine.laser) return { kind: "laser" };
    const index = machine.heads.findIndex((h) => h.xOffset === 0 && h.yOffset === 0);
    return index >= 0 ? { kind: "head", index } : { kind: "none" };
}

// ── offsets ──────────────────────────────────────────────────────────────────

/**
 * A head's centre relative to the anchor — fixed mounting geometry.
 *
 * This is the offset plan geometry needs, because `compileBlock` already bakes
 * the tool offset out (§3.2): baked coordinates are in head-centre space, and
 * this is what turns them into home-frame coordinates at emit time. WHICH head
 * to pass is a mount-table question the orchestrator answers at run time.
 */
export function headOffset(machine: MachineConfig, headIndex: number): XY {
    const head = machine.heads[headIndex];
    if (!head) {
        throw new RangeError(
            `headIndex ${headIndex} out of range (machine has ${machine.heads.length} head(s))`,
        );
    }
    return { x: head.xOffset, y: head.yOffset };
}

/**
 * A tool tip's position relative to the anchor — head mounting plus the tool's
 * own tip offset.
 *
 * This is the offset the jog path and the UI tip readout need: with no block to
 * name the tool, the mount table answers both "what is on this head" and "which
 * head", and the caller resolves the profile before calling.
 *
 * `profile` is required rather than optional so a call site cannot land in
 * head-centre space by omission — the two frames differ by exactly one tool
 * offset, a few millimetres, and agree everywhere else. For head-centre, call
 * `headOffset` and say so.
 */
export function toolFrameOffset(
    machine: MachineConfig,
    headIndex: number,
    profile: ToolProfile,
): XY {
    const head = headOffset(machine, headIndex);
    return {
        x: head.x + profile.toolOffset.xOffset,
        y: head.y + profile.toolOffset.yOffset,
    };
}

/**
 * How far apart two heads are mounted. The width a two-head job gives up from
 * the usable envelope, since the shared work area is the intersection of the
 * heads' reaches (§5.2).
 */
export function headSeparation(machine: MachineConfig, a: number, b: number): XY {
    const oa = headOffset(machine, a);
    const ob = headOffset(machine, b);
    return { x: Math.abs(ob.x - oa.x), y: Math.abs(ob.y - oa.y) };
}

// ── wire ↔ home ──────────────────────────────────────────────────────────────

/**
 * The per-axis calibration the wire conversions need. `AxisConfig` satisfies it
 * structurally, so a caller inside config passes the axis straight through and
 * a caller outside passes the same two fields `AxisCalibration` already holds.
 */
export interface AxisScale {
    readonly stepsPerUnit: number;
    /** Wiring inversion. The planner defines the frame; every consumer applies it. */
    readonly invert?: boolean;
}

/**
 * wire → home for one axis: step counter to mm (or degrees).
 *
 * Deliberately unrounded — this feeds a readout, and quantising it to whole
 * units would hide exactly the sub-millimetre drift the readout exists to show.
 */
export function stepsToUnits(steps: number, axis: AxisScale): number {
    return (steps / axis.stepsPerUnit) * (axis.invert ? -1 : 1);
}

/**
 * home → wire for one axis. Rounds, because a step count is an integer; this is
 * the same conversion `jogToPoint` applies to its targets.
 */
export function unitsToSteps(units: number, axis: AxisScale): number {
    return Math.round(units * axis.stepsPerUnit * (axis.invert ? -1 : 1));
}

/**
 * The machine's XY position in home frame, from a STATUS_RSP `pos` array
 * ([x, y, z, a] in wire steps).
 *
 * X/Y only: Z and A belong to a head, and reading them needs that head's own
 * axis rather than the machine's (§2). Feed the result to `homeToTool` to get
 * a tip reading.
 */
export function homePosition(machine: MachineConfig, wirePos: readonly number[]): XY {
    return {
        x: stepsToUnits(wirePos[0] ?? 0, machine.x),
        y: stepsToUnits(wirePos[1] ?? 0, machine.y),
    };
}

// ── transforms ───────────────────────────────────────────────────────────────

/**
 * home → tool: where the tip sits when the machine reads `pos`.
 *
 * `offset` comes from `headOffset` or `toolFrameOffset` — which one is the
 * caller's declaration of the frame it is working in.
 */
export function homeToTool(pos: XY, offset: XY): XY {
    return { x: pos.x + offset.x, y: pos.y + offset.y };
}

/**
 * tool → home: what to command so the tip lands on `target`.
 *
 * The go-to jog path: the operator names a tip coordinate, this says where the
 * machine has to be, and the wire-frame conversion happens downstream.
 */
export function toolToHome(target: XY, offset: XY): XY {
    return { x: target.x - offset.x, y: target.y - offset.y };
}
