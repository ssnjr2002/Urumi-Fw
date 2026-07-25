/**
 * link/commands.ts — control-plane command helpers over a Link.
 * Ported from host/protocol/commands.py, extended proactively with the
 * firmware verbs that landed since the Python snapshot.
 *
 * One function per text command from docs/wire_protocol.md "Control Plane",
 * plus the newer host verbs the firmware can answer today. Each sends the
 * command string and parses the reply so callers work with typed results
 * instead of raw text.
 *
 * `axisMap` is a designed-for stub — the firmware does not yet accept the
 * command (docs/engage_and_axis_map.md, PROPOSED), but the signature is the
 * one the eventual helper will have, so an orchestrator can code against it
 * now without a later API break.
 */

import { Link } from "./link.js";
import { parseGetstate, type MachineStatus } from "../format/status.js";

// ── query / liveness ─────────────────────────────────────────────────────────

/** USB link liveness check. */
export async function ping(link: Link): Promise<boolean> {
    return (await link.command("ping")) === "pong";
}

/**
 * Relay an RS485 ping to a bus node (or "all"). For a single node, true if the
 * text-plane reply ends with "ok"; for "all", true ONLY if every node answered.
 *
 * `all` answers on ONE line (`nodes 1=ok 2=timeout …`) — the text plane's
 * one-outstanding contract (D11). The Python port (`commands.py`) added the
 * per-node path; this retains it.
 */
export async function pingNode(link: Link, nodeId: number | "all"): Promise<boolean> {
    const reply = await link.command(`pingnode ${nodeId}`);
    if (reply.startsWith("nodes")) {
        const all = _parsePingAll(reply);
        return [...all.values()].every((v) => v);
    }
    return reply.endsWith("ok");
}

/** Ping every node in one command — one round trip. Returns {[nodeId]: answered}. */
export async function pingAll(link: Link): Promise<Map<number, boolean>> {
    return _parsePingAll(await link.command("pingnode all"));
}

// ── state / position ─────────────────────────────────────────────────────────

/** Operational status snapshot via the text `getstate` command. */
export async function getState(link: Link): Promise<MachineStatus> {
    return parseGetstate(await link.command("getstate"));
}

/** Binary status snapshot (STATUS_REQ / STATUS_RSP v2) — the cheaper poll. */
export function getStatus(link: Link): Promise<MachineStatus> {
    return link.getStatus();
}

/** Absolute machinePos in steps via the text `getpos` command: [x, y, z, a]. */
export async function getPos(link: Link): Promise<readonly [number, number, number, number]> {
    const r = await link.command("getpos");
    const parts = r.split(/\s+/);
    if (parts[0] !== "pos" || parts.length < 5) throw new Error(`bad getpos reply: ${JSON.stringify(r)}`);
    return [parseInt(parts[1]!), parseInt(parts[2]!), parseInt(parts[3]!), parseInt(parts[4]!)];
}

// ── node verbs (proactive — firmware additions since the Python snapshot) ────

/**
 * Query a single node's own step counter via `nodepos <id>`. The Pico relays
 * CMD_GET_POS over RS485 and replies with the node's int32 position — the
 * measurement CAN detect lost steps (unlike `getpos`, which reads what the
 * Pico THINKS it emitted). Reply format: `node <id> pos <steps>`.
 */
export async function nodePos(link: Link, nodeId: number): Promise<{ nodeId: number; pos: number }> {
    const r = await link.command(`nodepos ${nodeId}`);
    const m = /^node\s+(\d+)\s+pos\s+(-?\d+)/.exec(r);
    if (!m) throw new Error(`bad nodepos reply: ${JSON.stringify(r)}`);
    return { nodeId: parseInt(m[1]!), pos: parseInt(m[2]!) };
}

/**
 * Toggle a vacuum-node servo channel on/off.
 * Syntax: `vac_servo <node> <idx> <on|off>`.
 */
export async function vacServo(link: Link, nodeId: number, idx: number, on: boolean): Promise<boolean> {
    return _ok(link, `vac_servo ${nodeId} ${idx} ${on ? "on" : "off"}`);
}

/**
 * Turn the vacuum-node SSR pump on/off (soft-started on the node).
 * Syntax: `vac_pump <node> <on|off>`.
 */
export async function vacPump(link: Link, nodeId: number, on: boolean): Promise<boolean> {
    return _ok(link, `vac_pump ${nodeId} ${on ? "on" : "off"}`);
}

// ── enable / disable (per node or all) ────────────────────────────────────────

export function enable(link: Link, nodeId?: number): Promise<boolean> {
    const cmd = nodeId !== undefined ? `enable ${nodeId}` : "enable";
    return _ok(link, cmd);
}

export function disable(link: Link, nodeId?: number): Promise<boolean> {
    const cmd = nodeId !== undefined ? `disable ${nodeId}` : "disable";
    return _ok(link, cmd);
}

// ── motion control ────────────────────────────────────────────────────────────

export function setOrigin(link: Link, axes: string = ""): Promise<boolean> {
    return _ok(link, `setorigin ${axes}`.trimEnd());
}

export function pause(link: Link): Promise<boolean> {
    return _ok(link, "pause");
}

export function resume(link: Link): Promise<boolean> {
    return _ok(link, "resume");
}

export function cancel(link: Link): Promise<boolean> {
    return _ok(link, "cancel");
}

/** Emergency stop — always available. Confirmation arrives on the status sink. */
export function stop(link: Link): Promise<void> {
    return link.send("stop");
}

/** Clear ALARM → IDLE. */
export function unalarm(link: Link): Promise<boolean> {
    return _ok(link, "unalarm");
}

// ── axis_map — bind bus nodes to the four stream slots ───────────────────────

/** A slot binding: a bus id, or null for "leave this slot disengaged". */
export type SlotBinding = number | null;

/**
 * Declare the four motion-slot bindings (X, Y, Z, A) to the Pico, which
 * disengages every previously-bound node and engages each of these to its slot
 * via per-node CMD_ENGAGE. Not a diff — re-issuing the same map deliberately
 * re-sends every engage, so a node that silently lost its slot (reflash, power
 * blip, fresh Pico) is re-bound rather than skipped.
 *
 * Until a map commits the machine sits in ALARM/ALARM_CONFIG and refuses ALL
 * motion, so this is the first thing a host does after connecting — and the
 * thing it re-asserts on every reconnect, since the map is host-authored and
 * never appears in STATUS_RSP (docs/engage_and_axis_map.md §8).
 *
 * Throws with the firmware's reason on rejection (`err dup`, `err bad_node`,
 * `err bad_state`, `err node <id> timeout`) — the failure modes are distinct
 * enough that collapsing them to `false` would lose what the operator needs.
 * A partial failure leaves the committed map untouched; retrying redoes all of
 * it.
 *
 * Only valid in IDLE/PAUSED/ALARM — rebinding slots mid-RUNNING would corrupt
 * in-flight motion (§6.2).
 */
export async function axisMap(
    link: Link,
    x: SlotBinding,
    y: SlotBinding,
    z: SlotBinding,
    a: SlotBinding,
): Promise<boolean> {
    const tok = (n: SlotBinding): string => (n === null || n === 0 ? "-" : String(n));
    const reply = await link.command(`axis_map ${tok(x)} ${tok(y)} ${tok(z)} ${tok(a)}`);
    if (reply === "ok") return true;
    throw new Error(`axis_map rejected: ${reply || "no reply"}`);
}

/**
 * Read back the committed map as four slot bindings (null = unbound). The map
 * lives on Core 0 and is absent from STATUS_RSP by design, so this no-arg form
 * is the only way to observe it.
 */
export async function readAxisMap(link: Link): Promise<readonly [SlotBinding, SlotBinding, SlotBinding, SlotBinding]> {
    const r = await link.command("axis_map");
    const parts = r.split(/\s+/);
    if (parts[0] !== "axis_map" || parts.length < 5) {
        throw new Error(`bad axis_map reply: ${JSON.stringify(r)}`);
    }
    const one = (t: string): SlotBinding => (t === "-" || t === "0" ? null : parseInt(t, 10));
    return [one(parts[1]!), one(parts[2]!), one(parts[3]!), one(parts[4]!)];
}

// ── helpers ───────────────────────────────────────────────────────────────────

/**
 * Many control commands reply `ok` on success and `err <reason>` on
 * rejection or failure. Parse that into a boolean.
 */
async function _ok(link: Link, cmd: string): Promise<boolean> {
    const r = await link.command(cmd);
    if (r === "ok") return true;
    if (r.startsWith("err")) return false;
    return false; // unexpected reply — treated as failure
}

/**
 * Parse a `pingnode all` reply line: `nodes 1=ok 2=timeout 3=ok 4=timeout`
 * → Map { 1 → true, 2 → false, 3 → true, 4 → false }.
 */
function _parsePingAll(reply: string): Map<number, boolean> {
    const out = new Map<number, boolean>();
    const toks = reply.split(/\s+/);
    for (let i = 1; i < toks.length; i++) {
        const tok = toks[i]!;
        const eq = tok.indexOf("=");
        if (eq < 0) continue;
        const n = parseInt(tok.slice(0, eq), 10);
        const verdict = tok.slice(eq + 1);
        out.set(n, verdict === "ok");
    }
    return out;
}