/**
 * machine/setup.ts — what is fitted and engaged RIGHT NOW.
 *
 * The machine description says what the machine *is*: two head sockets, a Z and
 * an A node behind each, and a seed tool for each socket. It cannot say what is
 * screwed in this morning, or which head's Z/A currently hold wire slots 2 and
 * 3, because neither is a property of the machine — they change without the
 * config file changing, and the firmware does not report either (the axis map
 * is host-authored and absent from STATUS_RSP).
 *
 * That gap is why `schema.ts` documents `heads[].profile` and `defaultHead` as
 * "seed" values — a type apologising for carrying something it cannot vouch
 * for. Setup is the thing they are seeds *for*. Once a caller holds a Setup,
 * the description fields are read exactly once, at `setupFor()`, and never
 * again; everything downstream asks the Setup.
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
import type { ResolvedAxes } from "./resolve.js";

export interface Setup {
    /** Head whose Z/A occupy slots 2 and 3. Exactly one, always. */
    readonly engaged: number;
    /** Tool fitted in each head socket, by head index. null = empty socket. */
    readonly mounted: readonly (ToolProfile | null)[];
}

/**
 * Seed a Setup from the description: every socket holds its seed profile, and
 * `defaultHead` is engaged.
 *
 * This is the ONLY place those two fields are read. A freshly seeded Setup is
 * therefore exactly the assumption the library used to make implicitly
 * everywhere — which is what makes adopting Setup a no-op for existing callers
 * and an opt-in for anyone who wants the truth instead.
 */
export function setupFor(machine: MachineConfig): Setup {
    return {
        engaged: machine.defaultHead,
        mounted: machine.heads.map((h) => h.profile ?? null),
    };
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
    const mounted = setup.mounted.slice();
    mounted[head] = profile;
    return { ...setup, mounted };
}

/** The tool in the engaged head, or null if that socket is empty. */
export function engagedTool(setup: Setup): ToolProfile | null {
    return setup.mounted[setup.engaged] ?? null;
}

/** Head index holding `type` right now, or null. Contrast headAssignment(). */
export function headWithTool(setup: Setup, type: ToolType): number | null {
    const i = setup.mounted.findIndex((p) => p?.toolType === type);
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
 * This is `resolvedAxes()` with the live answer substituted for the static one.
 * resolvedAxes() resolves against `machine.defaultHead`, which is correct only
 * while the default head is the engaged one; on a dual-head machine after a
 * switch it silently returns the other head's calibration. That is not an
 * abstract risk — reading a live Z position through the wrong head's
 * stepsPerUnit is a clean 2x error on this bench machine, with no exception and
 * no wrong-looking number, just a wrong cut.
 */
export function setupAxes(machine: MachineConfig, setup: Setup): ResolvedAxes {
    const head = machine.heads[setup.engaged];
    if (!head) throw new RangeError(`setup engages head ${setup.engaged}, which does not exist`);
    return { x: machine.x, y: machine.y, z: head.z, a: head.a, fCpu: machine.fCpu };
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
        a.mounted.length === b.mounted.length &&
        a.mounted.every((p, i) => (p?.toolType ?? null) === (b.mounted[i]?.toolType ?? null))
    );
}
