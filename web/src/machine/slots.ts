/**
 * machine/slots.ts — the four wire slots, and which node fills each.
 *
 * A MicroSegment carries FOUR step deltas, not one per axis on the machine.
 * The Pico maps slot→node through the committed axis map, so on a dual-head
 * machine both heads' Z and A compete for slots 2 and 3 and only one head can
 * be engaged at a time. That constraint is the reason head selection is not a
 * UI preference: it decides what `axis_map` binds, and therefore which motors
 * move when a segment says "slot 2, +40 steps".
 *
 * Everything here is a pure function of the machine description — no Link, no
 * live state. Which head is *currently* engaged is a Setup concern (setup.ts);
 * this module only says what each choice would mean.
 */

import type { AxisConfig, MachineConfig, ToolHead, ToolType } from "./schema.js";

/** Slot index on the wire. X and Y are fixed; Z and A follow the engaged head. */
export const SLOT = { X: 0, Y: 1, Z: 2, A: 3 } as const;

/** One row of the machine's axis model: an axis, and the slot it would occupy. */
export interface AxisSlot {
    /** Stable identity across rebuilds: "x", "y", "h0z", "h1a". */
    readonly key: string;
    readonly letter: "x" | "y" | "z" | "a";
    readonly slot: number;
    /** "X", "Y", "Z0", "A1" — disambiguated by head where there is more than one. */
    readonly label: string;
    /** Head index for Z/A rows; undefined for the gantry axes. */
    readonly head?: number;
    readonly axis: AxisConfig;
    /** False when config says the node is not wired up. */
    readonly present: boolean;
    readonly stepsPerUnit: number;
    readonly invert: boolean;
    readonly unit: "mm" | "deg";
}

/**
 * Every axis the machine describes, gantry first then each head's Z/A.
 *
 * Absent nodes are INCLUDED, flagged `present: false`, rather than dropped: a
 * UI that filters them shows the operator an axis that silently does not exist,
 * where one rendered disabled says "the config declared this missing". Callers
 * that genuinely want only live axes filter on `present` themselves.
 */
export function axisSlots(machine: MachineConfig): readonly AxisSlot[] {
    const rows: Array<Omit<AxisSlot, "present" | "stepsPerUnit" | "invert" | "unit">> = [
        { key: "x", letter: "x", slot: SLOT.X, label: "X", axis: machine.x },
        { key: "y", letter: "y", slot: SLOT.Y, label: "Y", axis: machine.y },
    ];
    machine.heads.forEach((h, i) => {
        rows.push({ key: `h${i}z`, letter: "z", slot: SLOT.Z, label: `Z${i}`, head: i, axis: h.z });
        rows.push({ key: `h${i}a`, letter: "a", slot: SLOT.A, label: `A${i}`, head: i, axis: h.a });
    });
    return rows.map((r) => ({
        ...r,
        present: !!r.axis.node.present,
        stepsPerUnit: r.axis.stepsPerUnit,
        invert: !!r.axis.invert,
        unit: r.axis.rotary ? ("deg" as const) : ("mm" as const),
    }));
}

/** The four slot bindings, by bus node id. null = leave the slot disengaged. */
export type SlotMap = readonly [
    x: number | null,
    y: number | null,
    z: number | null,
    a: number | null,
];

/**
 * The bus ids this machine wants bound for `head`.
 *
 * Both heads' nodes exist on the bus; this picks the pair that will answer to
 * slots 2 and 3. An absent node binds as null so the slot stays disengaged
 * rather than aliasing onto whatever was there before.
 */
export function slotMapFor(machine: MachineConfig, head: number): SlotMap {
    const h: ToolHead | undefined = machine.heads[head];
    const id = (ax: AxisConfig | undefined): number | null =>
        ax && ax.node.present ? ax.node.id : null;
    return [id(machine.x), id(machine.y), id(h?.z), id(h?.a)];
}

/**
 * Which head a committed map corresponds to, or null for neither.
 *
 * The axis map is host-authored and never appears in STATUS_RSP
 * (docs/engage_and_axis_map.md §8), so reading it back and matching it against
 * each candidate is the ONLY way to learn which head the firmware currently
 * believes is engaged. null is a real answer — an unbound machine, or one bound
 * by some other host — and callers must handle it rather than defaulting to 0.
 */
export function headForSlotMap(
    machine: MachineConfig,
    committed: readonly (number | null)[] | null,
): number | null {
    if (!committed) return null;
    for (let i = 0; i < machine.heads.length; i++) {
        if (slotMapFor(machine, i).every((v, k) => v === committed[k])) return i;
    }
    return null;
}

/**
 * tool type → head index, from the heads' seed profiles.
 *
 * This answers "where would this tool be fitted", which is a description-level
 * question, and is how a mount schedule is turned into head switches. It is NOT
 * the same as "what is fitted right now" — see setup.ts. Later heads win on a
 * duplicate tool type, which only arises on a machine carrying two of the same
 * tool, where either answer is as good.
 */
export function headAssignment(machine: MachineConfig): ReadonlyMap<ToolType, number> {
    const m = new Map<ToolType, number>();
    machine.heads.forEach((h, i) => {
        if (h.profile) m.set(h.profile.toolType, i);
    });
    return m;
}
