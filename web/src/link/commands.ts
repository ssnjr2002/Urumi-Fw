/**
 * link/commands.ts — the text commands as methods (docs/reference/host-api/commands.md).
 *
 * One method per command in docs/reference/commands.md that a page needs.
 * Each sends its line through the link's one-outstanding text plane, reads
 * the reply, and resolves with what an `ok` carries or throws a CommandError.
 * `send()` is the escape hatch: any line, the reply unparsed.
 *
 * A reply says whether the controller took the command, never that what it
 * started has ended; `busy` in the status says whether something runs.
 * `onReady` waits for `busy` false before sending.
 */

import type { Link } from "./link.js";
import { Fault, parseGetReply } from "../packets/status.js";

/** A refused or failed command: the line, one reason token, the reply. */
export class CommandError extends Error {
    constructor(
        readonly command: string,
        readonly reason: string,
        readonly reply: string,
    ) {
        super(`${command}: ${reply || reason}`);
        this.name = "CommandError";
    }
}

/** A bus id per axis (X, Y, Z, A) or stream slot; null for none. */
export type AxisNodes = readonly [x: number | null, y: number | null, z: number | null, a: number | null];

/** `nodestat`: a field the node didn't send is undefined, never a default. */
export interface NodeStat {
    readonly id: number;
    readonly type: number;
    readonly enabled: boolean;
    readonly datum: boolean;
    readonly limit: boolean | undefined;
    readonly homing: boolean;
    readonly pos: number | undefined;
    readonly span: number | undefined;
    readonly index: number | undefined;
    readonly indexCause: string | undefined;
    readonly crossings: number | undefined;
    readonly stepsPerRev: number | undefined;
}

export type HomeTarget =
    | { nodes: readonly number[]; only?: boolean }
    | { cycle: number }
    | { head: number }
    | { form: "unhomed" | "xy" };

export interface Ramp {
    /** The step interval it starts at, µs. */
    readonly startUs: number;
    /** The interval it ramps to: the floor, or a probe leg's ceiling, µs. */
    readonly endUs: number;
    /** Steps the ramp takes. */
    readonly steps: number;
}

export type Leg =
    | {
          form: "seek" | "retract" | "sweep" | "span" | "unlatch";
          id: number;
          dir: 0 | 1;
          ramp: Ramp;
          maxSteps: number;
      }
    | { form: "park"; id: number; target: number; ramp: Ramp }
    | {
          form: "probe";
          id: number;
          dir: 0 | 1;
          ramp: Ramp;
          pollDiv: number;
          maxSteps: number;
          deadlineUs: number;
          expectOpen: boolean;
      };

export type JogDistance = { x?: number; y?: number } | { z: number } | { a: number };

type XY = number | "min" | "max";

export type GotoTarget =
    | { frame: "m" | "w"; x: XY; y: XY; scale?: number }
    | { frame: "m" | "w"; z: number; scale?: number }
    | { frame: "m" | "w"; a: number; scale?: number }
    | { name: "park" | "load" | "mount" | "probe"; scale?: number }
    | { name: "up" | "unlock" };

export type PlaceForm = "set" | "reset" | "frame" | "ok";
export interface PlaceCorner {
    x: "min" | "max";
    y: "min" | "max";
}

type Line = (line: string, timeoutMs?: number, tail?: string) => Promise<string>;

/** The commands that change something: `Commands` less `send` and the reads. */
export interface ReadyCommands {
    unalarm(): Promise<number>;
    reset(): Promise<void>;
    enable(id: number): Promise<void>;
    disable(id: number): Promise<void>;
    axesEnable(on: boolean): Promise<void>;
    axesMap(map: AxisNodes): Promise<void>;
    slotMap(map: AxisNodes): Promise<void>;
    vacServo(id: number, servo: number, on: boolean): Promise<void>;
    vacPump(id: number, on: boolean): Promise<void>;
    knifeOsc(id: number, on: boolean): Promise<void>;
    knifeBlower(id: number, duty: number): Promise<void>;
    home(target?: HomeTarget): Promise<void>;
    leg(leg: Leg): Promise<void>;
    legAbort(id: number): Promise<void>;
    setOrigin(pairs: readonly { id: number; steps: number }[]): Promise<void>;
    probeMap(vac: number): Promise<{ z: number; switchOpen: boolean }>;
    probeEnd(): Promise<boolean | null>;
    setProbe(zSteps: number): Promise<void>;
    unprobe(id?: number): Promise<void>;
    select(target: number | "anchor"): Promise<void>;
    wzero(axes?: readonly ("x" | "y" | "z" | "zSwitch")[]): Promise<void>;
    wset(offset: { x?: number; y?: number; z?: number }): Promise<void>;
    wclear(): Promise<void>;
    jog(by: JogDistance, options?: { scale?: number }): Promise<number>;
    goto(target: GotoTarget): Promise<void>;
    mount(head: number, tool: number): Promise<void>;
    unmount(head: number): Promise<void>;
    pause(): Promise<void>;
    cancel(): Promise<void>;
    place(form: PlaceForm): Promise<void>;
    place(form: "go", corner: PlaceCorner): Promise<void>;
}

const READY_METHODS = [
    "unalarm", "reset", "enable", "disable", "axesEnable", "axesMap", "slotMap",
    "vacServo", "vacPump", "knifeOsc", "knifeBlower", "home", "leg", "legAbort",
    "setOrigin", "probeMap", "probeEnd", "setProbe", "unprobe", "select", "wzero",
    "wset", "wclear", "jog", "goto", "mount", "unmount", "pause", "cancel", "place",
] as const satisfies readonly (keyof ReadyCommands)[];

/** Most keys one `get` takes (firmware GET_MAX_KEYS). */
const GET_MAX_KEYS = 32;
/** How long `reset` waits for its `ok`, ms. */
const RESET_REPLY_MS = 2000;

type AnyMethod = (...a: unknown[]) => Promise<unknown>;

export class Commands {
    /** The same commands, each sent once the status shows `busy` false. */
    readonly onReady: ReadyCommands;

    /** @internal Use link.commands(). */
    constructor(
        private readonly _line: Line,
        private readonly _link: Link,
    ) {
        const ready: Record<string, AnyMethod> = {};
        for (const name of READY_METHODS) {
            const m = (this[name] as AnyMethod).bind(this);
            ready[name] = async (...a: unknown[]) => {
                await this._untilFree(name);
                return m(...a);
            };
        }
        this.onReady = Object.freeze(ready) as unknown as ReadyCommands;
    }

    // ── any line ─────────────────────────────────────────────────────────────

    /** Any line; the reply unparsed, "" on a timeout. */
    async send(line: string, timeoutMs?: number): Promise<string> {
        const word = line.trim().split(/\s+/)[0];
        if (word === "stop") {
            this._link.estop();
            return "";
        }
        if (word === "reset" || word === "rst") return this._line(line, timeoutMs ?? RESET_REPLY_MS, "ready");
        return this._line(line, timeoutMs);
    }

    // ── reads ────────────────────────────────────────────────────────────────

    async ping(): Promise<void> {
        const r = await this._raw("ping");
        if (r !== "pong") throw this._error("ping", r);
    }

    async pingNode(id: number): Promise<void> {
        await this._node(`pingnode ${id}`);
    }

    async pingAll(): Promise<Map<number, string>> {
        const cmd = "pingnode all";
        const r = await this._raw(cmd);
        const toks = r.split(/\s+/);
        if (toks[0] !== "nodes") throw this._error(cmd, r);
        const out = new Map<number, string>();
        for (const tok of toks.slice(1)) {
            const eq = tok.indexOf("=");
            if (eq > 0) out.set(Number(tok.slice(0, eq)), tok.slice(eq + 1));
        }
        return out;
    }

    async get(keys: readonly string[]): Promise<Map<string, string>> {
        if (keys.length === 0) throw new RangeError("get needs at least one key");
        if (keys.length > GET_MAX_KEYS) throw new RangeError(`get takes at most ${GET_MAX_KEYS} keys`);
        const cmd = `get ${keys.join(" ")}`;
        const r = await this._raw(cmd);
        if (r.startsWith("err")) throw this._error(cmd, r);
        return parseGetReply(r);
    }

    async keys(): Promise<string[]> {
        const r = await this._raw("get");
        const parts = r.split(/\s+/);
        if (parts[0] !== "keys") throw this._error("get", r);
        return parts.slice(1);
    }

    async nodeStat(id: number): Promise<NodeStat> {
        const cmd = `nodestat ${id}`;
        const r = await this._raw(cmd);
        // `limit` is an optional group so the fields around it stay anchored.
        const m = /^node\s+(\d+)\s+type\s+(\d+)\s+en\s+(\d)\s+datum\s+(\d)\s+(?:limit\s+(\d)\s+)?homing\s+(\d)/.exec(r);
        if (!m) throw this._error(cmd, r);
        const num = (re: RegExp): number | undefined => {
            const x = re.exec(r);
            return x ? parseInt(x[1]!, 10) : undefined;
        };
        const ic = /\bidxcause\s+(\S+)/.exec(r);
        return {
            id: parseInt(m[1]!, 10),
            type: parseInt(m[2]!, 10),
            enabled: m[3] === "1",
            datum: m[4] === "1",
            limit: m[5] === undefined ? undefined : m[5] === "1",
            homing: m[6] === "1",
            pos: num(/\bpos\s+(-?\d+)/),
            span: num(/\bspan\s+(-?\d+)/),
            index: num(/\bindex\s+(-?\d+)/),
            indexCause: ic ? ic[1]! : undefined,
            crossings: num(/\bcross\s+(\d+)/),
            stepsPerRev: num(/\bsteprev\s+(-?\d+)/),
        };
    }

    async nodePos(id: number): Promise<number> {
        const cmd = `nodepos ${id}`;
        const r = await this._raw(cmd);
        const m = /^node\s+\d+\s+pos\s+(-?\d+)/.exec(r);
        if (!m) throw this._error(cmd, r);
        return parseInt(m[1]!, 10);
    }

    async vacSwitch(id: number): Promise<boolean> {
        const cmd = `vac_switch ${id}`;
        const r = await this._raw(cmd);
        const m = /^node\s+\d+\s+switch\s+(open|closed)/.exec(r);
        if (!m) throw this._error(cmd, r);
        return m[1] === "open";
    }

    // ── lifecycle ────────────────────────────────────────────────────────────

    async unalarm(): Promise<number> {
        const r = await this._ok("unalarm");
        const m = /^ok faults=(0x[0-9a-fA-F]+)/.exec(r);
        if (!m) throw this._error("unalarm", r);
        return parseInt(m[1]!, 16);
    }

    async reset(): Promise<void> {
        const r = await this._line("reset", RESET_REPLY_MS, "ready");
        if (r === "") throw new CommandError("reset", "timeout", "");
        if (r !== "ok") throw this._error("reset", r);
    }

    // ── bus and nodes ────────────────────────────────────────────────────────

    async enable(id: number): Promise<void> {
        await this._ok(`enable ${id}`);
    }

    async disable(id: number): Promise<void> {
        await this._ok(`disable ${id}`);
    }

    async axesEnable(on: boolean): Promise<void> {
        await this._ok(`axes_enable ${on ? "on" : "off"}`);
    }

    axesMap(): Promise<AxisNodes>;
    axesMap(map: AxisNodes): Promise<void>;
    axesMap(map?: AxisNodes): Promise<AxisNodes | void> {
        return this._map("axes_map", map);
    }

    slotMap(): Promise<AxisNodes>;
    slotMap(map: AxisNodes): Promise<void>;
    slotMap(map?: AxisNodes): Promise<AxisNodes | void> {
        return this._map("slot_map", map);
    }

    // ── peripherals ──────────────────────────────────────────────────────────

    async vacServo(id: number, servo: number, on: boolean): Promise<void> {
        await this._node(`vac_servo ${id} ${servo} ${on ? "on" : "off"}`);
    }

    async vacPump(id: number, on: boolean): Promise<void> {
        await this._node(`vac_pump ${id} ${on ? "on" : "off"}`);
    }

    async knifeOsc(id: number, on: boolean): Promise<void> {
        await this._node(`knife_osc ${id} ${on ? "on" : "off"}`);
    }

    async knifeBlower(id: number, duty: number): Promise<void> {
        if (!(duty >= 0 && duty <= 100)) throw new RangeError(`blower duty ${duty} is not 0 to 100`);
        await this._node(`knife_blower ${id} ${Math.round(duty)}`);
    }

    // ── homing ───────────────────────────────────────────────────────────────

    async home(target?: HomeTarget): Promise<void> {
        let words: string[];
        if (target === undefined) words = [];
        else if ("nodes" in target) words = [...(target.only ? ["only"] : []), ...target.nodes.map(String)];
        else if ("cycle" in target) words = ["cycle", String(target.cycle)];
        else if ("head" in target) words = ["head", String(target.head)];
        else words = [target.form];
        await this._ok(["home", ...words].join(" "));
    }

    async leg(leg: Leg): Promise<void> {
        const r = leg.ramp;
        const ramp = `${r.startUs} ${r.endUs} ${r.steps}`;
        const words =
            leg.form === "park"
                ? `${leg.target} ${ramp}`
                : leg.form === "probe"
                  ? `${leg.dir} ${ramp} ${leg.pollDiv} ${leg.maxSteps} ${leg.deadlineUs} ${leg.expectOpen ? 1 : 0}`
                  : `${leg.dir} ${ramp} ${leg.maxSteps}`;
        await this._ok(`leg ${leg.form} ${leg.id} ${words}`);
    }

    async legAbort(id: number): Promise<void> {
        await this._ok(`leg_abort ${id}`);
    }

    async setOrigin(pairs: readonly { id: number; steps: number }[]): Promise<void> {
        await this._ok(`setorigin ${pairs.map((p) => `${p.id}:${Math.round(p.steps)}`).join(" ")}`);
    }

    // ── probe ────────────────────────────────────────────────────────────────

    async probeMap(vac: number): Promise<{ z: number; switchOpen: boolean }> {
        const cmd = `probe_map ${vac}`;
        const r = await this._ok(cmd);
        const m = /\bz=(\d+).*\bswitch=([01])/.exec(r);
        if (!m) throw this._error(cmd, r);
        return { z: parseInt(m[1]!, 10), switchOpen: m[2] === "1" };
    }

    async probeEnd(): Promise<boolean | null> {
        const r = await this._ok("probe_end");
        const m = /\bswitch=([01?])/.exec(r);
        if (!m) throw this._error("probe_end", r);
        return m[1] === "?" ? null : m[1] === "1";
    }

    async setProbe(zSteps: number): Promise<void> {
        await this._ok(`setprobe ${Math.round(zSteps)}`);
    }

    async unprobe(id?: number): Promise<void> {
        await this._ok(id === undefined ? "unprobe" : `unprobe ${id}`);
    }

    // ── positioning ──────────────────────────────────────────────────────────

    async select(target: number | "anchor"): Promise<void> {
        await this._ok(`select ${target}`);
    }

    async wzero(axes: readonly ("x" | "y" | "z" | "zSwitch")[] = []): Promise<void> {
        await this._ok(["wzero", ...axes.map((a) => (a === "zSwitch" ? "z_switch" : a))].join(" "));
    }

    async wset(offset: { x?: number; y?: number; z?: number }): Promise<void> {
        const parts = (["x", "y", "z"] as const)
            .filter((k) => offset[k] !== undefined)
            .map((k) => `${k} ${offset[k]}`);
        if (parts.length === 0) throw new RangeError("wset needs at least one axis");
        await this._ok(`wset ${parts.join(" ")}`);
    }

    async wclear(): Promise<void> {
        await this._ok("wclear");
    }

    async jog(by: JogDistance, options: { scale?: number } = {}): Promise<number> {
        const b = by as { x?: number; y?: number; z?: number; a?: number };
        const xy = b.x !== undefined || b.y !== undefined;
        const sets = (xy ? 1 : 0) + (b.z !== undefined ? 1 : 0) + (b.a !== undefined ? 1 : 0);
        if (sets !== 1) throw new RangeError("jog moves exactly one axis set: XY, Z or A");
        const body = xy
            ? b.x !== undefined && b.y !== undefined
                ? `xy ${b.x} ${b.y}`
                : b.x !== undefined ? `x ${b.x}` : `y ${b.y}`
            : b.z !== undefined ? `z ${b.z}` : `a ${b.a}`;
        const scale = options.scale !== undefined && options.scale !== 1 ? ` ${options.scale}` : "";
        const cmd = `jog ${body}${scale}`;
        const r = await this._ok(cmd);
        const m = /^ok (\d+)$/.exec(r);
        if (!m) throw this._error(cmd, r);
        return Number(m[1]);
    }

    async goto(t: GotoTarget): Promise<void> {
        const scale = "scale" in t && t.scale !== undefined && t.scale !== 1 ? ` ${t.scale}` : "";
        const body =
            "name" in t
                ? t.name
                : "x" in t
                  ? `${t.frame} xy ${t.x} ${t.y}`
                  : "z" in t
                    ? `${t.frame} z ${t.z}`
                    : `${t.frame} a ${t.a}`;
        await this._ok(`goto ${body}${scale}`);
    }

    // ── tools ────────────────────────────────────────────────────────────────

    async mount(head: number, tool: number): Promise<void> {
        await this._ok(`mount ${head} ${tool}`);
    }

    async unmount(head: number): Promise<void> {
        await this._ok(`unmount ${head}`);
    }

    // ── jobs ─────────────────────────────────────────────────────────────────

    async pause(): Promise<void> {
        await this._ok("pause");
    }

    async cancel(): Promise<void> {
        await this._ok("cancel");
    }

    place(form: PlaceForm): Promise<void>;
    place(form: "go", corner: PlaceCorner): Promise<void>;
    async place(form: PlaceForm | "go", corner?: PlaceCorner): Promise<void> {
        if (form !== "go") return void (await this._ok(`place ${form}`));
        if (!corner) throw new RangeError("place go needs a corner");
        await this._ok(`place go x${corner.x} y${corner.y}`);
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    /** The reply, or a CommandError for no reply. */
    private async _raw(cmd: string): Promise<string> {
        const r = await this._line(cmd);
        if (r === "") throw new CommandError(cmd, "timeout", "");
        return r;
    }

    /** `ok` or `ok <detail>`, returned whole; anything else throws. */
    private async _ok(cmd: string): Promise<string> {
        const r = await this._raw(cmd);
        if (r === "ok" || r.startsWith("ok ")) return r;
        throw this._error(cmd, r);
    }

    /** A relay: `node <id> ok`, else the node's result throws. */
    private async _node(cmd: string): Promise<void> {
        const r = await this._raw(cmd);
        if (/^node\s+\d+\s+ok$/.test(r)) return;
        throw this._error(cmd, r);
    }

    /** One reason token from a reply that isn't the success it should be. */
    private _error(cmd: string, reply: string): CommandError {
        const t = reply.split(/\s+/);
        let reason = "bad_reply";
        if (t[0] === "err") {
            // `err node <id> <result>`: the node's result is the reason.
            reason = t[1] === "node" && t[3] !== undefined ? t[3] : (t[1] ?? "bad_reply");
        } else if (t[0] === "node" && t[2] !== undefined) {
            reason = t[2];
        }
        return new CommandError(cmd, reason, reply);
    }

    private async _map(word: "axes_map" | "slot_map", map?: AxisNodes): Promise<AxisNodes | void> {
        if (map !== undefined) {
            await this._ok(`${word} ${map.map((n) => (n === null ? "-" : String(n))).join(" ")}`);
            return;
        }
        const r = await this._raw(word);
        const t = r.split(/\s+/);
        if (t[0] !== word || t.length < 5) throw this._error(word, r);
        // `?n` pending, `!n` fenced: the node is still the one named.
        const one = (s: string): number | null => {
            const n = parseInt(s.replace(/^[?!]/, ""), 10);
            return Number.isFinite(n) && n > 0 ? n : null;
        };
        return [one(t[1]!), one(t[2]!), one(t[3]!), one(t[4]!)];
    }

    /** Wait for a sample with `busy` false; throw on a latched e-stop. */
    private async _untilFree(name: string): Promise<void> {
        let s = this._link.status ?? (await this._link.freshStatus());
        for (;;) {
            if (s.hasFault(Fault.ESTOP)) throw new CommandError(name, "estop", "");
            if (!s.busy) return;
            s = await this._link.freshStatus();
        }
    }
}
