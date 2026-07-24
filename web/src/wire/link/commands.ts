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

// ── designed-for: axis_map (PROPOSED, not yet implemented in firmware) ────────

/**
 * Declare the four motion-slot bindings to the Pico. The Pico owns the
 * current map and diffs each new `axis_map` into the minimal set of
 * per-node CMD_ENGAGE packets.
 *
 * NOT YET IMPLEMENTED — the firmware does not accept this command yet
 * (docs/engage_and_axis_map.md, Workstream A). Bindings are always 4 nodes
 * (0 = no axis on that slot); a value of 0 leaves the slot disengaged.
 */
export async function axisMap(
    _link: Link,
    _x: number,
    _y: number,
    _z: number,
    _a: number,
): Promise<boolean> {
    throw new Error(
        "axis_map: not yet implemented on the firmware side " +
        "(docs/engage_and_axis_map.md, Workstream A)",
    );
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