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
 * A node's full state via `nodestat <id>`, including the homing leg span.
 *
 * `span` is how far that node's last COMPLETED homing leg actually moved, in
 * its own steps, measured by the node itself (src/node/types/stepper/stepper.cpp).
 * It is per LEG and not per home: the node sees individual `home` commands and
 * has no idea they form a sequence, so this is whichever leg finished last. The
 * caller is the one that knows leg 1 was the seek and that its span is the frame.
 *
 * `undefined` when the node did not send it — a board that cannot home has no
 * leg to measure. Never defaulted to 0, which is a real reading meaning "armed
 * and went nowhere".
 *
 * `index` is the ROTARY answer, and it is deliberately NOT `pos`. A limit
 * switch's edge IS the position, so a linear seek stops on its datum; an analog
 * dip's centre is only knowable after passing it, so a rotary sweep runs THROUGH
 * the feature and halts somewhere past it. Both numbers are real and neither
 * substitutes for the other.
 *
 * `indexCause` is present whenever the node has an index at all — including
 * "none" before the first sweep — while `index` appears only when that cause is
 * "ok". A sweep that found nothing has no index, and 0 is a legitimate step
 * coordinate rather than a sentinel. Both `undefined` on a linear node.
 *
 * `limit` is ABSENT on a node whose homing kind is not a limit switch, and that
 * absence is the point: printing `limit 0` for a node with no switch states a
 * fact about a thing that does not exist, and reads as a switch that is fine
 * rather than one that is not there. So it is optional here and surfaces as
 * `undefined`, never as `false` -- a caller that cannot tell "clear" from
 * "absent" is exactly the caller this distinction exists for.
 *
 * `cross` and `steprev` are the rotary sweep's EVIDENCE, not its answer.
 * `cross` is how many times the sweep passed the index; it is reported even on
 * a failure, where `cross 0` against `idxcause notfound` says the magnet was
 * never seen at all rather than that the budget was short. `steprev` is the
 * mean interval between those crossings -- a measured steps-per-revolution,
 * only meaningful once at least two crossings exist.
 *
 * Reply: `node <id> type <t> en <b> datum <b> [limit <b>] homing <b> pos <p> slot <s>
 *         [span <n>] [idxcause <word> [index <n>] hall <n> base <n>]
 *         [cross <n> [steprev <n>]]`
 */
export async function nodeStat(link: Link, nodeId: number): Promise<{
    nodeId: number; type: number; enabled: boolean; datum: boolean;
    limit: boolean | undefined; homing: boolean; pos: number;
    span: number | undefined;
    index: number | undefined; indexCause: string | undefined;
    crossings: number | undefined; stepsPerRev: number | undefined;
}> {
    const r = await link.command(`nodestat ${nodeId}`);
    // `limit` is an optional GROUP rather than a separate probe so the fields
    // around it stay anchored: `homing` follows it either way, and a loose
    // /limit\s+(\d)/ searched anywhere in the line would let a genuinely
    // malformed reply through while happily matching nothing on a rotary node.
    const m = /^node\s+(\d+)\s+type\s+(\d+)\s+en\s+(\d)\s+datum\s+(\d)\s+(?:limit\s+(\d)\s+)?homing\s+(\d)\s+pos\s+(-?\d+)/.exec(r);
    if (!m) throw new Error(`bad nodestat reply: ${JSON.stringify(r)}`);
    const sp = /\bspan\s+(-?\d+)/.exec(r);
    const ic = /\bidxcause\s+(\S+)/.exec(r);
    const ix = /\bindex\s+(-?\d+)/.exec(r);
    const cr = /\bcross\s+(\d+)/.exec(r);
    const sr = /\bsteprev\s+(-?\d+)/.exec(r);
    return {
        nodeId: parseInt(m[1]!), type: parseInt(m[2]!),
        enabled: m[3] === "1", datum: m[4] === "1",
        limit: m[5] === undefined ? undefined : m[5] === "1",
        homing: m[6] === "1",
        pos: parseInt(m[7]!),
        span: sp ? parseInt(sp[1]!, 10) : undefined,
        index: ix ? parseInt(ix[1]!, 10) : undefined,
        indexCause: ic ? ic[1]! : undefined,
        crossings: cr ? parseInt(cr[1]!, 10) : undefined,
        stepsPerRev: sr ? parseInt(sr[1]!, 10) : undefined,
    };
}

/**
 * Toggle a vacuum-node servo channel on/off.
 * Syntax: `vac_servo <node> <idx> <on|off>`.
 */
export async function vacServo(link: Link, nodeId: number, idx: number, on: boolean): Promise<boolean> {
    return _nodeOk(link, `vac_servo ${nodeId} ${idx} ${on ? "on" : "off"}`);
}

/**
 * Turn the vacuum-node SSR pump on/off (soft-started on the node).
 * Syntax: `vac_pump <node> <on|off>`.
 */
export async function vacPump(link: Link, nodeId: number, on: boolean): Promise<boolean> {
    return _nodeOk(link, `vac_pump ${nodeId} ${on ? "on" : "off"}`);
}

/**
 * Read the vacuum node's NC switch. Reply: `node <id> switch open|closed (level=N)`.
 * The switch is wired to GND with a pull-up, so closed (level 0) is rest and
 * open (level 1) is actuated. Returns null if the node did not answer.
 */
export async function vacSwitch(link: Link, nodeId: number): Promise<boolean | null> {
    const r = await link.command(`vac_switch ${nodeId}`);
    const m = /^node\s+\d+\s+switch\s+(open|closed)/.exec(r);
    return m ? m[1] === "open" : null;
}

// ── knife peripheral (oscillating drag knife) ────────────────────────────────

/**
 * Toggle the knife node's oscillator. Syntax: `knife_osc <node> <on|off>`.
 *
 * Refused while RUNNING — the firmware gates every peripheral relay on
 * IDLE/PAUSED/ALARM (control_plane.cpp), because the relay blocks Core 0 on a
 * Core 1 round trip that would otherwise interleave with the stream. Mid-job
 * control therefore has to happen at a PAUSED boundary.
 */
export function knifeOsc(link: Link, nodeId: number, on: boolean): Promise<boolean> {
    return _nodeOk(link, `knife_osc ${nodeId} ${on ? "on" : "off"}`);
}

/**
 * Set the knife node's blower PWM duty, 0..100 %.
 * Syntax: `knife_blower <node> <0..100>`. Same RUNNING restriction as knifeOsc.
 */
export function knifeBlower(link: Link, nodeId: number, dutyPct: number): Promise<boolean> {
    const duty = Math.max(0, Math.min(100, Math.round(dutyPct)));
    return _nodeOk(link, `knife_blower ${nodeId} ${duty}`);
}

// ── enable / disable ──────────────────────────────────────────────────────────
// Per node (any bus id, type-blind relay) or, with no id, across the axis map.
// The map form is `axes_enable on|off`: peripherals hold no motion slot, so they
// are reachable only by an explicit id.

export function enable(link: Link, nodeId?: number): Promise<boolean> {
    const cmd = nodeId !== undefined ? `enable ${nodeId}` : "axes_enable on";
    return _ok(link, cmd);
}

export function disable(link: Link, nodeId?: number): Promise<boolean> {
    const cmd = nodeId !== undefined ? `disable ${nodeId}` : "axes_enable off";
    return _ok(link, cmd);
}

export function axesEnable(link: Link, on: boolean): Promise<boolean> {
    return _ok(link, `axes_enable ${on ? "on" : "off"}`);
}

// ── motion control ────────────────────────────────────────────────────────────

/**
 * Record a datum: the named axes' current physical position IS `posSteps`.
 *
 * `posSteps` defaults to 0 — "here is the origin" — which is the bare-jog case.
 * A home needs the other form: after leg 4 the axis is parked a known distance
 * clear of a switch whose own machine coordinate is known, so the datum is that
 * arithmetic, not zero. Omitting it there would put the origin at the park
 * point and silently shift the whole coordinate system by parkMm.
 *
 * Answers `err unbound` if NO named axis resolved to a node — a mask where
 * nothing resolved recorded nothing, and `ok` there would report a datum that
 * does not exist.
 */
export function setOrigin(link: Link, axes: string = "", posSteps?: number): Promise<boolean> {
    const args = posSteps !== undefined ? `${axes} ${posSteps}` : axes;
    return _ok(link, `setorigin ${args}`.trim());
}

// ── homing ────────────────────────────────────────────────────────────────────

/**
 * Arm ONE LEG on bus node `node` and return as soon as the Pico has armed it.
 *
 * Deliberately a single leg, not a sequence — and the verb says so. The
 * firmware command is one leg: the node runs the motion and stops itself, Core 0
 * only supervises the waiting. A full linear home is four of these plus a
 * `setorigin` (docs/homing.md §3.4); a rotary home is two plus a `setorigin` and
 * a `moveto`. The sequencing lives in `homing/`, which owns the arithmetic; this
 * stays a thin verb so a bring-up console can drive a single leg by hand.
 *
 * ADDRESSES A BUS ID, not an axis letter. Everything a leg produces is
 * node-framed — the span, the index in the node own counter, the limit latch —
 * so the firmware refuses to route it through the axis map. The practical gain
 * is that a leg runs BEFORE any `axis_map` is committed, which is exactly when
 * a new head is being commissioned.
 *
 * `ok` means ARMED, not finished. The machine is now in HOMING and the caller
 * must poll `getstate` until it leaves — see homing/sequence.ts.
 *
 * The Pico probes the node kind before arming and answers `err kind_mismatch`
 * if the verb does not match what the node declares, so a `linLeg` aimed at a
 * rotary head refuses without moving anything.
 *
 * @param node      RS485 bus id.
 * @param dir       1 or 0 — the node own direction sense, not a signed axis
 *                  direction. Derive it with approachDir(); `invert` is already
 *                  folded in there.
 * @param startUs   step interval the leg starts at
 * @param floorUs   interval it ramps down to (== startUs for an un-ramped leg)
 * @param rampSteps steps taken to get from startUs to floorUs
 * @param maxSteps  runaway budget.
 */
/** Result of arming a leg: whether it armed, and — on refusal — why. */
export interface HomeResult {
    armed: boolean;
    /** The raw `err <reason>` text (e.g. "err kind_mismatch node 4 is 2 want 1", "err bad_state"), omitted on success. */
    reason?: string;
}

/**
 * One leg of a LINEAR (limit-switch) home.
 *
 * `retract` does not STEER the node — it still picks seek or retract itself
 * from a single read of its own switch pin at arm time, and the host cannot
 * know that pin state ahead of the command (§1.2). What it does is let the
 * node catch a divergence between the plan model and physical reality: this
 * plan leg has an expectation (`HomingLeg.kind`), the node checks it against
 * the pin, and a mismatch is a NAK (NAK_INTENT_MISMATCH) rather than a leg run
 * under the wrong budget semantics — a seek runaway cap executed to completion
 * as a retract, ignoring the switch it was meant to stop at.
 *
 * @param retract   this leg expected mode — true for one planned to start
 *                  already on the switch (§3.4 legs 2 and 4), false for one
 *                  planned to start clear (legs 1 and 3). From `HomingLeg.kind`.
 * @param maxSteps  a seek stops at the switch and this is only a cap; a retract
 *                  IGNORES the switch and travels EXACTLY this many steps, which
 *                  is what makes leg 4 distance knowable.
 */
export async function linLeg(
    link: Link,
    node: number,
    dir: 0 | 1,
    retract: boolean,
    startUs: number,
    floorUs: number,
    rampSteps: number,
    maxSteps: number,
): Promise<HomeResult> {
    const intent = retract ? 1 : 0;
    const r = await link.command(
        `lin_leg ${node} ${dir} ${startUs} ${floorUs} ${rampSteps} ${maxSteps} ${intent}`,
    );
    if (r === "ok") return { armed: true };
    return { armed: false, reason: r };
}

/**
 * One leg of a ROTARY (Hall index) home.
 *
 * No `intent`, because a rotary node has no limit pin: there is nothing for the
 * host to predict and nothing for the node to disagree with. The leg is
 * evidence-terminated — it runs until it has crossed the magnet enough times to
 * prove the period — so `maxSteps` here is a pure runaway ceiling and never a
 * tuning knob. Give it about 4x the expected steps per revolution: a sweep that
 * starts just past the index needs three full laps.
 *
 * The answer is not where the axis stopped. Read `index` and `steprev` out of
 * `nodeStat` once the machine leaves HOMING.
 */
export async function rotLeg(
    link: Link,
    node: number,
    dir: 0 | 1,
    startUs: number,
    floorUs: number,
    rampSteps: number,
    maxSteps: number,
): Promise<HomeResult> {
    const r = await link.command(
        `rot_leg ${node} ${dir} ${startUs} ${floorUs} ${rampSteps} ${maxSteps}`,
    );
    if (r === "ok") return { armed: true };
    return { armed: false, reason: r };
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
/**
 * Node relays do NOT reply `ok` — they reply `node <id> ok` or `node <id>
 * timeout`, because the answer is about a node on the RS485 bus and not about
 * the Pico. Parsing these with `_ok` reads every success as a failure.
 */
async function _nodeOk(link: Link, cmd: string): Promise<boolean> {
    return /^node\s+\d+\s+ok$/.test(await link.command(cmd));
}

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