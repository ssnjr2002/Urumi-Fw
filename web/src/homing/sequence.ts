/**
 * homing/sequence.ts — run a derived plan against a Link.
 *
 * All the arithmetic already happened in derive.ts. What is left is the part
 * that needs a machine: send a leg, wait for the Pico to leave HOMING, and
 * check the verdict against what the leg said it expected.
 *
 * The waiting exists because `leg` returns as soon as the leg is ARMED, not
 * when it finishes — deliberately, since a leg takes seconds and a handler that
 * blocked would freeze `get` and every abort for the whole seek
 * (docs/homing.md §2.3). The host therefore owns the polling, and the sequence
 * is four arm-and-wait rounds followed by one `setorigin`.
 */

import type { Link } from "../wire/link/link.js";
import {
    linLeg, rotLeg, setOrigin, getState, readAxisMap, nodeStat,
} from "../wire/link/commands.js";
import { settle, SettleError } from "../wire/link/settled.js";
import {
    MachineState,
    AXIS_BITS,
    type MachineStatus,
} from "../wire/format/status.js";
import {
    LegKind, type HomingLeg, type HomingPlan, type RotaryHomingPlan,
} from "./types.js";
import { resolveRotaryIndex } from "./derive.js";

export interface RunHomingOptions {
    /** Gap between status polls, ms. */
    pollMs?: number;
    /**
     * Per-leg ceiling, ms. 0 = wait forever. A leg has its own runaway budget on
     * the Pico AND a derived deadline in the supervisor, so this is a third
     * backstop for the case both of those miss: a link that stopped answering.
     */
    legTimeoutMs?: number;
    /** Called before each leg is armed — for a progress line. */
    onLeg?: (leg: HomingLeg, index: number, total: number) => void;
    /** Called with the terminal status of each leg. */
    onLegDone?: (leg: HomingLeg, status: MachineStatus) => void;
}

/** A leg ended somewhere the plan did not expect. Carries the terminal sample. */
export class HomingError extends Error {
    constructor(
        readonly leg: HomingLeg | null,
        readonly status: MachineStatus | null,
        message: string,
    ) {
        super(message);
        this.name = "HomingError";
    }
}

/**
 * Arm one leg and wait for it to finish. Returns the terminal status.
 *
 * `fatal: []` on the settle is essential and not a relaxation: legs 1 and 3 are
 * SUPPOSED to end in ALARM/LIMIT_LATCHED, and settle()'s default treats ALARM
 * as a reason to abandon the wait. Leaving the default in would abort every
 * successful seek. The verdict is judged below instead, where the difference
 * between "alarmed because it found the switch" and "alarmed because it did
 * not" is actually available.
 */
async function runLeg(
    link: Link,
    node: number,
    leg: HomingLeg,
    opts: RunHomingOptions,
): Promise<MachineStatus> {
    // What the plan expects this leg to find at arm time: BACKOFF and PARK
    // start ON the switch (they exist to retract off it); SEEK and LATCH start
    // clear. The node checks this against its own pin read and NAKs on
    // disagreement rather than running the leg under the wrong budget
    // semantics (docs/homing.md §1.4, §2.6).
    const intendedRetract = leg.kind === LegKind.BACKOFF || leg.kind === LegKind.PARK;
    const { armed, reason } = await linLeg(
        link, node, leg.dir, intendedRetract,
        leg.startUs, leg.floorUs, leg.rampSteps, leg.maxSteps,
    );
    if (!armed) {
        throw new HomingError(leg, null,
            `${leg.axis}: Pico refused to arm the ${leg.kind} leg (${reason ?? "no reply"})`);
    }

    const st = await awaitLeg(link, leg, opts);

    // Judge the axis's own bit, not the machine state. With two axes homing in
    // sequence, X parked and clear while Y is mid-seek leaves the machine in
    // ALARM/LIMIT_LATCHED — correctly — and a check on machineState alone would
    // read that as X having failed.
    // `?? 0` covers firmware predating the `latched=` token, not a binary
    // sample: getState() is the text plane and always carries the field.
    const bit = AXIS_BITS[leg.axis];
    const latched = bit !== undefined && !!((st.axesLatched ?? 0) & bit);
    if (latched !== leg.endsLatched) {
        throw new HomingError(leg, st,
            `${leg.axis} ${leg.kind}: expected the switch ` +
            `${leg.endsLatched ? "held" : "clear"} at the end of this leg, ` +
            `but latched=0x${(st.axesLatched ?? 0).toString(16)}`);
    }
    return st;
}

/**
 * Wait out a leg that is already armed and decode its verdict.
 *
 * Shared by both kinds, and everything in it is kind-agnostic: a leg either
 * finished or it failed, and the failure codes name where to look rather than
 * what shape of home this was. Only the LATCH check afterwards is linear-only,
 * which is why it stays in runLeg — a rotary node has no switch to be standing
 * on, so asking whether it is would always answer the same thing.
 */
async function awaitLeg(
    link: Link,
    leg: HomingLeg,
    opts: RunHomingOptions,
): Promise<MachineStatus> {
    try {
        await settle(link, (s) => s.state !== MachineState.HOMING, {
            pollMs: opts.pollMs ?? 150,
            timeoutMs: opts.legTimeoutMs ?? 0,
            fatal: [],
        });
    } catch (e) {
        const se = e as SettleError;
        throw new HomingError(leg, se.status, `${leg.axis} ${leg.kind}: ${se.message}`);
    }

    // The verdict comes from the TEXT plane, not the binary poll settle() used.
    // Only `get` carries the per-axis `latched` mask — STATUS_RSP has no
    // room for it — and the mask is what distinguishes "this axis is on its
    // switch" from "some axis is". One extra round trip per leg, four per home.
    const st = await getState(link);
    opts.onLegDone?.(leg, st);

    return st;
}

/**
 * Run a full LINEAR home: four legs, then the datum.
 *
 * The closing `setOrigin` is part of the sequence and not an afterthought. A
 * home moves the axis with the NODE's pulser, which Core 1 does not count, so
 * the Pico drops the axis's datum at the start of every leg — the machine is
 * un-homed until this line runs, by design. Skipping it leaves an axis that
 * moved to a known place and cannot say where that is.
 */
export async function runHoming(
    link: Link,
    plan: HomingPlan,
    opts: RunHomingOptions = {},
): Promise<void> {
    // Resolve the axis to a bus id ONCE, here. The legs are node-addressed
    // (commands.ts `leg`) because everything they produce is node-framed, but
    // a PLAN is written against an axis — datumSteps is a machine coordinate,
    // and only the map says which motor that axis is. Reading the committed map
    // rather than trusting a cached one matters because the map is host-authored
    // and absent from STATUS_RSP: a reconnect against a Pico someone else
    // remapped would otherwise home the wrong head.
    //
    // Once, not per leg, so a remap landing mid-sequence cannot move legs 3 and
    // 4 to a different motor than legs 1 and 2 — which would leave the first
    // head parked on a switch with nobody about to retract it.
    const map = await readAxisMap(link);
    const slot = "xyza".indexOf(plan.axis.toLowerCase());
    const node = slot < 0 ? null : map[slot];
    if (node === null || node === undefined) {
        throw new HomingError(null, null,
            `${plan.axis}: no node is bound to that axis — commit an axis_map first`);
    }

    for (let i = 0; i < plan.legs.length; i++) {
        const leg = plan.legs[i]!;
        opts.onLeg?.(leg, i, plan.legs.length);
        await runLeg(link, node, leg, opts);
    }
    if (!(await setOrigin(link, [{ node, steps: plan.datumSteps }]))) {
        throw new HomingError(null, null,
            `${plan.axis}: legs completed but setorigin ${plan.datumSteps} was refused ` +
            `— the axis is parked correctly but has no datum`);
    }
}

// ── rotary ──────────────────────────────────────────────────────────

/** What one sweep measured, read straight out of `nodestat` after the leg. */
export interface SweepResult {
    /** The index, in the NODE own step counter. Not where the axis stopped. */
    readonly index: number;
    /** Measured steps per revolution — the mean interval between crossings. */
    readonly stepsPerRev: number;
    /** How many times the sweep passed the index. */
    readonly crossings: number;
    /** The node counter after the sweep, i.e. where the axis actually is. */
    readonly pos: number;
}

/** What a rotary home worked out, returned so an operator can see the evidence. */
export interface RotaryHomingResult {
    readonly forward: SweepResult;
    readonly reverse: SweepResult;
    /**
     * Half the folded separation between the two answers, in steps — the
     * one-way measurement bias that averaging removes. Signed, and its SIGN is
     * a property of the mechanism, not of the run.
     */
    readonly biasSteps: number;
    /** The averaged index, in the node counter frame. */
    readonly indexSteps: number;
    /** Mean of the two measured steps-per-revolution. */
    readonly stepsPerRev: number;
    /** What `setorigin` was given: the machine coordinate of where it stopped. */
    readonly originSteps: number;
}

/** Read the sweep evidence off a node, insisting every field is really there. */
async function readSweep(
    link: Link,
    node: number,
    leg: HomingLeg,
): Promise<SweepResult> {
    const st = await nodeStat(link, node);
    // The leg already passed awaitLeg, so the supervisor saw no failure. A
    // missing field here therefore is not a failed sweep — it is a node that
    // reported success and then did not carry the answer, which is a firmware
    // mismatch and worth saying so rather than reading as a bad magnet.
    if (st.indexCause !== "ok" || st.index === undefined
        || st.stepsPerRev === undefined || st.crossings === undefined) {
        throw new HomingError(leg, null,
            `${leg.axis}: node ${node} reported the sweep finished but carried no `
            + `usable index (idxcause ${st.indexCause ?? "absent"}, `
            + `cross ${st.crossings ?? "absent"}) — check the node firmware matches `
            + `this host`);
    }
    return {
        index: st.index,
        stepsPerRev: st.stepsPerRev,
        crossings: st.crossings,
        pos: st.pos,
    };
}

/**
 * Run a full ROTARY home: two sweeps, then the datum.
 *
 * The datum is NOT computed in advance, which is the structural difference from
 * runHoming(). A linear plan knows where leg 4 parks before anything moves; a
 * rotary sweep whole output IS the position, so the origin can only be worked
 * out once both legs have reported.
 *
 * There is no closing move. The axis is left wherever the second sweep post-roll
 * put it, and `setorigin` names that spot in machine coordinates — so the axis
 * is homed while standing at some arbitrary angle, which is legitimate for an
 * axis with no ends. Driving to the datum afterwards is the host business and
 * belongs in a normal `moveto`, where it gets acceleration, soft limits and
 * coordination that a bespoke closing burst would not.
 */
export async function runRotaryHoming(
    link: Link,
    plan: RotaryHomingPlan,
    opts: RunHomingOptions = {},
): Promise<RotaryHomingResult> {
    // Resolved once, for the same reason runHoming() resolves once: a remap
    // landing between the two sweeps would average an index measured on one
    // motor against one measured on another.
    const map = await readAxisMap(link);
    const slot = "xyza".indexOf(plan.axis.toLowerCase());
    const node = slot < 0 ? null : map[slot];
    if (node === null || node === undefined) {
        throw new HomingError(null, null,
            `${plan.axis}: no node is bound to that axis — commit an axis_map first`);
    }

    const results: SweepResult[] = [];
    for (let i = 0; i < plan.legs.length; i++) {
        const leg = plan.legs[i]!;
        opts.onLeg?.(leg, i, plan.legs.length);
        const { armed, reason } = await rotLeg(
            link, node, leg.dir,
            leg.startUs, leg.floorUs, leg.rampSteps, leg.maxSteps,
        );
        if (!armed) {
            throw new HomingError(leg, null,
                `${plan.axis}: Pico refused to arm the `
                + `${leg.dir === 1 ? "forward" : "reverse"} sweep `
                + `(${reason ?? "no reply"})`);
        }
        await awaitLeg(link, leg, opts);
        // Read the evidence BEFORE the next leg arms: the node keeps one
        // sweep worth of answer and the next arm overwrites it.
        results.push(await readSweep(link, node, leg));
    }

    const [forward, reverse] = results as [SweepResult, SweepResult];
    const { stepsPerRev, revSpread, biasSteps, indexSteps } = resolveRotaryIndex(
        forward.index, forward.stepsPerRev,
        reverse.index, reverse.stepsPerRev,
    );

    // The two measured periods are independent measurements of the same
    // mechanism. Disagreeing means one of the sweeps counted something that is
    // not once-per-revolution, and averaging two such answers hides it.
    if (revSpread > stepsPerRev * 0.02) {
        throw new HomingError(null, null,
            `${plan.axis}: the two sweeps measured different revolutions `
            + `(${forward.stepsPerRev} vs ${reverse.stepsPerRev} steps) — more than `
            + `2% apart. The axis slipped, or something other than the index magnet `
            + `is being counted`);
    }

    if (Math.abs(biasSteps) > plan.toleranceSteps) {
        const deg = (biasSteps / plan.stepsPerUnit).toFixed(2);
        const tolDeg = (plan.toleranceSteps / plan.stepsPerUnit).toFixed(2);
        throw new HomingError(null, null,
            `${plan.axis}: the forward and reverse sweeps disagree by ${deg} deg `
            + `(tolerance ${tolDeg} deg). A degree or so is the normal one-way bias; `
            + `much more than that means the magnet moved, the belt slipped, or `
            + `stepsPerUnit is wrong`);
    }
    // Where the axis is STANDING, in machine coordinates. The index is the
    // measured point and `datumSteps` is the coordinate it has been assigned, so
    // everything else follows by offset. setorigin cannot name a point the axis
    // is not on — it records "the node counter right now corresponds to this"
    // — which is exactly why the arithmetic is here and not on the wire.
    const originSteps = Math.round(plan.datumSteps + (reverse.pos - indexSteps));
    if (!(await setOrigin(link, [{ node, steps: originSteps }]))) {
        throw new HomingError(null, null,
            `${plan.axis}: both sweeps completed but setorigin ${originSteps} was `
            + `refused — the index is measured but the axis has no datum`);
    }

    return {
        forward, reverse, biasSteps, indexSteps, stepsPerRev, originSteps,
    };
}
