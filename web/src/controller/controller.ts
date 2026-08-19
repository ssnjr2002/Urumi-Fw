/**
 * controller/controller.ts — the one object allowed to know both the machine
 * description and the live link.
 *
 * Everything below this file is deliberately one or the other. The bake tower
 * (machine → svg → toolpath → choreograph → production → plan → orchestrate) is
 * pure and offline; the transport tower (wire/format → wire/link) knows bytes
 * and states and has never heard of a tool. That separation is worth keeping —
 * it is why the planner is testable without a port and the Link is testable
 * without a config. But three questions cannot be answered from either side
 * alone, and every UI built on this library ends up answering them itself:
 *
 *   A. Exclusivity. The ack sink is one shared resource. A jog and a job
 *      streaming at once corrupt the sequence window, and `Link` cannot stop
 *      them because it deliberately does not know what a session is *for*.
 *
 *   B. Setup reconciliation. The axis map never appears in STATUS_RSP. The
 *      firmware knows which node drives which slot; the host knows which head
 *      carries which tool. Neither can answer "is this machine set up to run
 *      this plan?" alone.
 *
 *   C. Frame-correct live position. Turning a wire position into tool-frame mm
 *      needs the head that is engaged *right now*. `frames.ts` has the maths and
 *      no way to know that; reading Z through the wrong head's calibration is a
 *      silent 2x error on this bench machine — no exception, just a wrong cut.
 *
 * Those three are correctness. Everything else here is convenience built on
 * them.
 *
 * WHAT THIS DOES NOT DO. It does not bake (that stays pure and offline), frame
 * packets, own a transport, or render anything. There is no DOM, no `setInterval`
 * the caller cannot replace, and no callback that fires from a timer the caller
 * did not start. It emits state; the UI decides what that looks like.
 */

import type { MachineConfig, ToolProfile, ToolType } from "../machine/schema.js";
import type { ResolvedAxes } from "../machine/resolve.js";
import {
    setupFor,
    engage,
    mount,
    engagedTool,
    setupAxes,
    setupSlotMap,
    isCommitted,
    adoptCommitted,
    sameSetup,
    type Setup,
} from "../machine/setup.js";
import type { SlotMap } from "../machine/slots.js";
import {
    homePosition,
    headOffset,
    toolFrameOffset,
    homeToTool,
    stepsToUnits,
    type XY,
} from "../machine/frames.js";
import { Link } from "../wire/link/link.js";
import { axisMap, readAxisMap, type SlotBinding } from "../wire/link/commands.js";
import {
    settle,
    atRest,
    type SettleCondition,
    type SettleOptions,
} from "../wire/link/settled.js";
import { MachineState, type MachineStatus } from "../wire/format/status.js";
import { stateName } from "../wire/format/names.js";

// ── exclusivity (invariant A) ────────────────────────────────────────────────

/**
 * Thrown when an operation that needs the ack sink finds it already held.
 *
 * Carrying `holder` matters more than it looks: "busy" alone sends the operator
 * hunting for a phantom, where "busy: job" tells them the thing they forgot is
 * still streaming.
 */
export class BusyError extends Error {
    constructor(readonly holder: string) {
        super(`controller is busy: ${holder}`);
        this.name = "BusyError";
    }
}

/**
 * A held claim on the ack sink. Release it when the work is done — from a
 * `finally`, always, because a leaked lease locks the machine out until the page
 * reloads, which looks exactly like a hung transport.
 */
export interface Lease {
    readonly kind: string;
    /** False once released. Releasing twice is a no-op, not an error. */
    readonly active: boolean;
    release(): void;
}

// ── events ───────────────────────────────────────────────────────────────────

export interface ControllerEvents {
    /** Every status sample, from a poll or an explicit refresh. */
    status: MachineStatus;
    /** The Setup changed — a head switch, a mount, or an adopt on connect. */
    setup: Setup;
    /** The committed axis map was read back or re-committed. */
    committed: SlotMap | null;
    /** A lease was taken or released. Null means the machine is free. */
    busy: string | null;
    /**
     * Something failed in the background, where there is no caller to throw at
     * — in practice a poll against a port that went away. Unhandled, these are
     * swallowed; that is deliberate, since a dead poll must not become an
     * unhandled rejection, but a UI should show it.
     */
    error: Error;
}

type Handler<K extends keyof ControllerEvents> = (value: ControllerEvents[K]) => void;

export interface ControllerOptions {
    /** Gap between background polls, ms. Default 250. */
    pollMs?: number;
    /** Injectable clock and sleep — the only timing this class does. */
    now?: () => number;
    sleep?: (ms: number) => Promise<void>;
}

// ── the controller ───────────────────────────────────────────────────────────

export class Controller {
    readonly machine: MachineConfig;
    readonly link: Link;

    private _setup: Setup;
    private _status: MachineStatus | null = null;
    private _committed: SlotMap | null = null;
    private _lease: { kind: string; active: boolean } | null = null;
    private _polling = false;
    private _pollLoop: Promise<void> | null = null;

    private readonly _pollMs: number;
    private readonly _now: () => number;
    private readonly _sleep: (ms: number) => Promise<void>;
    private readonly _handlers = new Map<keyof ControllerEvents, Set<(v: never) => void>>();

    constructor(machine: MachineConfig, link: Link, options: ControllerOptions = {}) {
        this.machine = machine;
        this.link = link;
        this._setup = setupFor(machine);
        this._pollMs = options.pollMs ?? 250;
        this._now = options.now ?? (() => Date.now());
        this._sleep = options.sleep ?? ((ms) => new Promise<void>((r) => setTimeout(r, ms)));
    }

    // -- observation ----------------------------------------------------------

    /** What is fitted and engaged right now. Immutable; compare with `sameSetup`. */
    get setup(): Setup {
        return this._setup;
    }

    /** The most recent status sample, or null if none has been taken yet. */
    get status(): MachineStatus | null {
        return this._status;
    }

    /** The firmware's committed slot map as last read, or null if unread/unbound. */
    get committed(): SlotMap | null {
        return this._committed;
    }

    /**
     * Does the firmware agree with our Setup? False means a commit is owed —
     * never "probably fine". An unread map is not a match (see `isCommitted`).
     */
    get synced(): boolean {
        return isCommitted(this.machine, this._setup, this._committed);
    }

    /** What holds the ack sink, or null. */
    get busy(): string | null {
        return this._lease?.active ? this._lease.kind : null;
    }

    get closed(): boolean {
        return this.link.closed;
    }

    /** The four axes with Z/A taken from the ENGAGED head — invariant C's fix. */
    get axes(): ResolvedAxes {
        return setupAxes(this.machine, this._setup);
    }

    /** The tool in the engaged head, or null if that socket is empty. */
    get tool(): ToolProfile | null {
        return engagedTool(this._setup);
    }

    /**
     * tool type → head socket, for handing to `walkSchedule`.
     *
     * DYING. Compiled blocks will carry their own head (docs/head_binding.md
     * stage 4), which removes both this getter and walkSchedule's option. Until
     * then it reads the LIVE setup rather than the config — config no longer
     * claims to know where a tool sits, and where it sits right now is the only
     * answer a runtime rebind can act on. A tool fitted nowhere is absent, and
     * the caller's `?? 0` covers it exactly as before.
     */
    get headAssignment(): ReadonlyMap<ToolType, number> {
        const m = new Map<ToolType, number>();
        this._setup.mounts.forEach((p, i) => {
            if (p && !m.has(p.toolType)) m.set(p.toolType, i);
        });
        return m;
    }

    // -- events ---------------------------------------------------------------

    /** Subscribe. Returns the unsubscribe function. */
    on<K extends keyof ControllerEvents>(event: K, handler: Handler<K>): () => void {
        let set = this._handlers.get(event);
        if (!set) {
            set = new Set();
            this._handlers.set(event, set);
        }
        set.add(handler as (v: never) => void);
        return () => {
            set.delete(handler as (v: never) => void);
        };
    }

    private _emit<K extends keyof ControllerEvents>(event: K, value: ControllerEvents[K]): void {
        const set = this._handlers.get(event);
        if (!set) return;
        for (const h of [...set]) {
            // One subscriber throwing must not stop the others, and must not
            // fail the operation that emitted. A render bug is not a comms bug.
            try {
                (h as Handler<K>)(value);
            } catch {
                /* subscriber's problem */
            }
        }
    }

    // -- invariant A: exclusivity ---------------------------------------------

    /**
     * Claim the ack sink. Throws BusyError if something else holds it.
     *
     * Everything that writes packets — a jog session, a go-to, a job stream —
     * must hold a lease for its whole span. Text commands and status polls do
     * NOT need one: they route on their own magic into their own sinks and are
     * safe alongside a stream by design (that is the point of the demux).
     */
    acquire(kind: string): Lease {
        if (this._lease?.active) throw new BusyError(this._lease.kind);
        const held = { kind, active: true };
        this._lease = held;
        this._emit("busy", kind);
        return {
            kind,
            get active() {
                return held.active;
            },
            release: () => {
                if (!held.active) return;
                held.active = false;
                if (this._lease === held) this._lease = null;
                this._emit("busy", this.busy);
            },
        };
    }

    /** Run `fn` under a lease, releasing it however `fn` ends. */
    async withLease<T>(kind: string, fn: (lease: Lease) => Promise<T>): Promise<T> {
        const lease = this.acquire(kind);
        try {
            return await fn(lease);
        } finally {
            lease.release();
        }
    }

    /**
     * Emergency stop. Deliberately bypasses the lease and the writer's command
     * queue: an estop that waits its turn behind a pending text command is not
     * an estop. Confirmation arrives on the status sink as ESTOP → ALARM.
     */
    async estop(): Promise<void> {
        await this.link.send("stop");
    }

    /**
     * Soft abort (§4.5): ramp to rest, flush the ring, land IDLE with position
     * intact. Also lease-free — the whole reason to call it is that something
     * else is running.
     */
    abort(): void {
        this.link.abort();
    }

    // -- status ---------------------------------------------------------------

    /** One round trip. Updates `status` and emits. */
    async refresh(): Promise<MachineStatus> {
        const st = await this.link.getStatus();
        this._status = st;
        this._emit("status", st);
        return st;
    }

    /**
     * Start polling in the background. An awaited loop, not `setInterval`: two
     * polls can never overlap, and a slow port stretches the gap instead of
     * queueing requests behind each other.
     */
    startPolling(pollMs = this._pollMs): void {
        if (this._polling) return;
        this._polling = true;
        this._pollLoop = (async () => {
            while (this._polling && !this.link.closed) {
                try {
                    await this.refresh();
                } catch (e) {
                    this._emit("error", e instanceof Error ? e : new Error(String(e)));
                }
                if (!this._polling) break;
                await this._sleep(pollMs);
            }
            this._polling = false;
        })();
    }

    /** Stop polling and wait for the in-flight poll to finish. */
    async stopPolling(): Promise<void> {
        this._polling = false;
        const loop = this._pollLoop;
        this._pollLoop = null;
        if (loop) await loop;
    }

    /**
     * Poll until `until` holds. Samples flow to `status` subscribers on the way,
     * so a UI keeps painting during a wait without the caller wiring `onPoll`.
     */
    settle(until: SettleCondition, options: SettleOptions = {}): Promise<MachineStatus> {
        const { onPoll, ...rest } = options;
        return settle(
            this.link,
            until,
            {
                ...rest,
                now: rest.now ?? this._now,
                sleep: rest.sleep ?? this._sleep,
                onPoll: (st) => {
                    this._status = st;
                    this._emit("status", st);
                    onPoll?.(st);
                },
            },
        );
    }

    /** True if the machine came to rest inside `timeoutMs`. Never throws for a fault. */
    async waitAtRest(timeoutMs = 5000): Promise<boolean> {
        try {
            await this.settle(atRest, { timeoutMs, fatal: [], pollMs: 50 });
            return true;
        } catch {
            return false;
        }
    }

    // -- invariant B: setup reconciliation ------------------------------------

    /**
     * Read the firmware's committed map back. The map lives on Core 0 and is
     * absent from STATUS_RSP by design, so this round trip is the ONLY way to
     * observe it. A failed read records null — an unknown map is not a match.
     */
    async readCommitted(): Promise<SlotMap | null> {
        try {
            const m = await readAxisMap(this.link);
            this._committed = m as SlotMap;
        } catch {
            this._committed = null;
        }
        this._emit("committed", this._committed);
        return this._committed;
    }

    /**
     * Adopt whatever the firmware is bound to, if it is recognisable.
     *
     * The first thing to do on connect, before assuming anything. The map
     * survives in the Pico across a host reload, so a fresh page has no idea
     * which head is engaged — and guessing `defaultHead` then fighting the
     * machine is worse than asking. Returns false when the map matches no head,
     * which means the caller's next move is `commit()`, not another guess.
     */
    async sync(): Promise<boolean> {
        await this.readCommitted();
        const adopted = adoptCommitted(this.machine, this._setup, this._committed);
        if (!sameSetup(adopted, this._setup)) {
            this._setup = adopted;
            this._emit("setup", adopted);
        }
        return this.synced;
    }

    /**
     * Bind this setup's slots on the firmware, optionally engaging `head` first.
     *
     * Until a map commits, the Pico sits in ALARM/ALARM_CONFIG and NACKs every
     * job, jog and debug step, so this is the first thing a host does after
     * connecting and the thing it re-asserts on every reconnect. Not a diff:
     * re-issuing the same map deliberately re-sends every engage, so a node that
     * silently lost its slot (reflash, power blip) is re-bound rather than
     * skipped.
     *
     * Refused while RUNNING. Rebinding slots mid-motion would land the incoming
     * head's Z/A on the outgoing head's motors, which is the failure the whole
     * axis-map mechanism exists to prevent; the legal windows are IDLE, PAUSED
     * and ALARM, and a head switch belongs at a tool-change pause.
     */
    async commit(head?: number): Promise<void> {
        const target = head ?? this._setup.engaged;
        const next = engage(this.machine, this._setup, target);

        const st = this._status;
        if (st && st.state === MachineState.RUNNING) {
            throw new Error(
                `cannot rebind the axis map while ${stateName(st.state)} — ` +
                "pause or wait for rest first",
            );
        }

        const [x, y, z, a] = setupSlotMap(this.machine, next) as readonly SlotBinding[];
        await axisMap(this.link, x!, y!, z!, a!);

        if (!sameSetup(next, this._setup)) {
            this._setup = next;
            this._emit("setup", next);
        }
        await this.readCommitted();
    }

    /**
     * Engage a head host-side WITHOUT touching the firmware.
     *
     * The pending-vs-committed split is why Setup is a value: a UI can show
     * "head 1 selected, commit owed" by engaging here and reading `synced`.
     * Callers that want the machine to follow call `commit(head)` instead.
     */
    engage(head: number): Setup {
        const next = engage(this.machine, this._setup, head);
        if (next !== this._setup) {
            this._setup = next;
            this._emit("setup", next);
        }
        return next;
    }

    /** Record what the operator physically fitted (or null to empty the socket). */
    mount(head: number, profile: ToolProfile | null): Setup {
        const next = mount(this.machine, this._setup, head, profile);
        this._setup = next;
        this._emit("setup", next);
        return next;
    }

    // -- invariant C: frame-correct live position -----------------------------

    /**
     * The machine's XY in home frame, from a status sample.
     *
     * Defaults to the latest poll; pass one explicitly to convert a sample taken
     * at a particular instant. Returns null when no sample carries a position —
     * a text-plane status has none, and inventing (0,0) there would read as the
     * machine sitting at the origin.
     */
    homeXY(status: MachineStatus | null = this._status): XY | null {
        const pos = status?.pos;
        return pos ? homePosition(this.machine, pos) : null;
    }

    /**
     * Where the engaged head's CENTRE is, in tool frame.
     *
     * Separate from `tipXY` on purpose: the two differ by one tool offset — a
     * few millimetres — and agree everywhere else, so a caller that lands in the
     * wrong one by omission gets a plausible number that is quietly wrong.
     */
    headXY(status: MachineStatus | null = this._status): XY | null {
        const home = this.homeXY(status);
        return home && homeToTool(home, headOffset(this.machine, this._setup.engaged));
    }

    /**
     * Where the engaged tool's TIP is. Throws if that socket is empty, because
     * there is no tip to report and head centre is not a synonym for one.
     */
    tipXY(status: MachineStatus | null = this._status): XY | null {
        const profile = engagedTool(this._setup);
        if (!profile) {
            throw new Error(`head ${this._setup.engaged} has no tool fitted — no tip to report`);
        }
        const home = this.homeXY(status);
        return home && homeToTool(home, toolFrameOffset(this.machine, this._setup.engaged, profile));
    }

    /**
     * One axis of the live position in its own units (mm, or degrees for A),
     * read through the ENGAGED head's calibration.
     *
     * This is invariant C in one line. `resolvedAxes()` would answer with
     * `defaultHead`'s numbers, which is right until someone switches heads and
     * then silently wrong by whatever ratio separates the two Z motors.
     */
    axisUnits(letter: "x" | "y" | "z" | "a", status: MachineStatus | null = this._status): number | null {
        const pos = status?.pos;
        if (!pos) return null;
        const index = { x: 0, y: 1, z: 2, a: 3 }[letter];
        return stepsToUnits(pos[index] ?? 0, this.axes[letter]);
    }

    // -- lifecycle ------------------------------------------------------------

    /**
     * Stop polling and close the transport. The Setup survives — it describes
     * what is physically screwed into the machine, which a disconnect does not
     * change — but the committed map does not, since we can no longer see it.
     */
    async close(): Promise<void> {
        await this.stopPolling();
        this._committed = null;
        this._emit("committed", null);
        try {
            await this.link.close();
        } catch {
            /* already gone */
        }
    }
}
