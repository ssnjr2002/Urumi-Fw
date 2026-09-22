/**
 * machine/setup.ts — what is fitted and engaged RIGHT NOW.
 *
 * The machine description says what the machine *is*: two head sockets, a Z and
 * an A node behind each, and the tools each socket's fixture accepts. It cannot
 * say what is screwed in this morning, or which head's Z/A currently hold wire
 * slots 2 and 3, because neither is a property of the machine — they change
 * without the config file changing, and the firmware does not report either
 * (the axis map is host-authored and absent from STATUS_RSP).
 *
 * Setup is that live half. `accepts` says the knife CAN go here; Setup says the
 * knife IS here. The description is read exactly once, at `setupFor()`, to pick
 * a plausible starting arrangement; everything downstream asks the Setup.
 *
 * Setup is not what the bake reads. Which head a block cuts on is decided by
 * the scheduler from `accepts` (docs/head_binding.md), so a compiled block
 * already names its head. The Controller's job is to check a Setup AGAINST that
 * decision, never to supply it.
 *
 * Setup is a VALUE, not a session: immutable, cheap to copy, comparable, and
 * holding no Link. Every operation returns a new Setup. That is what lets a UI
 * keep a pending setup beside the committed one and diff them — the "you have
 * unsaved changes to the axis map" case — which a mutable object makes fiddly.
 *
 * What Setup deliberately does NOT do is talk to the machine. Committing an
 * axis map, reading one back, or refusing to switch heads mid-RUNNING are
 * Controller concerns; Setup only computes what such a commit would mean.
 */

import type { AxisConfig, MachineConfig, ToolProfile, ToolType } from "./schema.js";
import { slotMapFor, headForSlotMap, type SlotMap } from "./slots.js";
import { TOOL_PROFILES_BY_TYPE } from "./tools.js";
import { axesForHead, type ResolvedAxes } from "./resolve.js";

export interface Setup {
    /** Head whose Z/A occupy slots 2 and 3. Exactly one, always. */
    readonly engaged: number;
    /** Tool fitted in each head socket, by head index. null = empty socket. */
    readonly mounts: readonly (ToolProfile | null)[];
}

/**
 * Seed a Setup from the description: every socket holds its most preferred
 * acceptable tool, and `defaultHead` is engaged.
 *
 * `accepts[0]` is a guess, and an honest one — it is the arrangement the config
 * says this machine would rather be in, which is the best available answer
 * before anyone has looked at the machine. A socket that accepts nothing seeds
 * empty. Callers who know better replace it; callers who need certainty read
 * the machine and adopt what it says.
 */
export function setupFor(machine: MachineConfig): Setup {
    return {
        engaged: machine.defaultHead,
        mounts: machine.heads.map((h) => profileForType(h.accepts[0])),
    };
}

function profileForType(type: ToolType | undefined): ToolProfile | null {
    return type === undefined ? null : TOOL_PROFILES_BY_TYPE[type] ?? null;
}

/**
 * The mount table as tool TYPES — what a scheduler takes.
 *
 * `scheduleMounts` reasons about which tool sits in which socket and never
 * touches a profile's kinematics, so it keys on ToolType. This is the one
 * conversion between the live table and that argument.
 */
export function mountedTypes(setup: Setup): readonly (ToolType | null)[] {
    return setup.mounts.map((p) => p?.toolType ?? null);
}

/** Engage a different head. Throws on an index the machine does not have. */
export function engage(machine: MachineConfig, setup: Setup, head: number): Setup {
    if (!machine.heads[head]) {
        throw new RangeError(`no head ${head} (machine has ${machine.heads.length})`);
    }
    return setup.engaged === head ? setup : { ...setup, engaged: head };
}

/** Fit `profile` (or null to empty the socket) into `head`. */
export function mount(
    machine: MachineConfig,
    setup: Setup,
    head: number,
    profile: ToolProfile | null,
): Setup {
    if (!machine.heads[head]) {
        throw new RangeError(`no head ${head} (machine has ${machine.heads.length})`);
    }
    const mounts = setup.mounts.slice();
    mounts[head] = profile;
    return { ...setup, mounts };
}

/** The tool in the engaged head, or null if that socket is empty. */
export function engagedTool(setup: Setup): ToolProfile | null {
    return setup.mounts[setup.engaged] ?? null;
}

/**
 * Head index holding `type` right now, or null.
 *
 * Contrast `headsAccepting()`, which answers where it COULD go. This answers
 * where it is.
 */
export function headWithTool(setup: Setup, type: ToolType): number | null {
    const i = setup.mounts.findIndex((p) => p?.toolType === type);
    return i < 0 ? null : i;
}

/** Is `type` fitted anywhere at all? */
export function isMounted(setup: Setup, type: ToolType): boolean {
    return headWithTool(setup, type) !== null;
}

// ── what the setup means downstream ──────────────────────────────────────────

/**
 * The four axes as the wire sees them, with Z/A taken from the ENGAGED head.
 *
 * This is `axesForHead()` pointed at the ENGAGED head — the live-state entry
 * point, for reading back a position or driving an operator jog. Compiled work
 * does not come through here: a block names its own head and resolves against
 * that, whether or not it is the one currently engaged.
 *
 * Using `resolvedAxesDefault()` here instead would be correct only while the
 * default head is the engaged one; on a dual-head machine after a switch it
 * silently returns the other head's calibration. Reading a live Z position
 * through the wrong head's stepsPerUnit is a clean 2x error on this bench
 * machine, with no exception and no wrong-looking number.
 */
export function setupAxes(machine: MachineConfig, setup: Setup): ResolvedAxes {
    return axesForHead(machine, setup.engaged);
}

/** One axis of the engaged head by letter — the frame-conversion entry point. */
export function engagedAxis(
    machine: MachineConfig,
    setup: Setup,
    letter: "x" | "y" | "z" | "a",
): AxisConfig {
    return setupAxes(machine, setup)[letter];
}

/** The slot bindings this setup implies. What a commit would send. */
export function setupSlotMap(machine: MachineConfig, setup: Setup): SlotMap {
    return slotMapFor(machine, setup.engaged);
}

/**
 * Does the firmware's committed map agree with this setup?
 *
 * The reconciliation the Controller owns, reduced to a function. `false` means
 * a commit is owed — either nothing has been bound since connect (the machine
 * sits in ALARM_CONFIG and NACKs everything), or a head switch has been made
 * host-side and not yet pushed. It never means "probably fine": an unreadable
 * or unbound map is not a match.
 */
export function isCommitted(
    machine: MachineConfig,
    setup: Setup,
    committed: readonly (number | null)[] | null,
): boolean {
    return headForSlotMap(machine, committed) === setup.engaged;
}

/**
 * Adopt whatever the firmware is actually bound to, if it is recognisable.
 *
 * On connect the host has no idea which head is engaged — the map survives in
 * the Pico across a host reload. Reading it back and adopting it is better than
 * assuming defaultHead and then fighting the machine. Returns the setup
 * unchanged when the map matches nothing, so the caller's next step is to
 * commit rather than to guess.
 */
export function adoptCommitted(
    machine: MachineConfig,
    setup: Setup,
    committed: readonly (number | null)[] | null,
): Setup {
    const head = headForSlotMap(machine, committed);
    return head === null ? setup : engage(machine, setup, head);
}

/** Value equality — for "is the pending setup different from the live one". */
export function sameSetup(a: Setup, b: Setup): boolean {
    return (
        a.engaged === b.engaged &&
        a.mounts.length === b.mounts.length &&
        a.mounts.every((p, i) => (p?.toolType ?? null) === (b.mounts[i]?.toolType ?? null))
    );
}
