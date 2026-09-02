/**
 * homing/sequence.ts — run a derived plan against a Link.
 *
 * All the arithmetic already happened in derive.ts. What is left is the part
 * that needs a machine: send a leg, wait for the Pico to leave HOMING, and
 * check the verdict against what the leg said it expected.
 *
 * The waiting exists because `home` returns as soon as the leg is ARMED, not
 * when it finishes — deliberately, since a leg takes seconds and a handler that
 * blocked would freeze `getstate` and every abort for the whole seek
 * (docs/homing.md §2.3). The host therefore owns the polling, and the sequence
 * is four arm-and-wait rounds followed by one `setorigin`.
 */

import type { Link } from "../wire/link/link.js";
import { home, setOrigin, getState } from "../wire/link/commands.js";
import { settle, SettleError } from "../wire/link/settled.js";
import {
    MachineState,
    AlarmReason,
    HomeFail,
    AXIS_BITS,
    type MachineStatus,
} from "../wire/format/status.js";
import { alarmName } from "../wire/format/names.js";
import { LegKind, type HomingLeg, type HomingPlan } from "./types.js";

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
    leg: HomingLeg,
    opts: RunHomingOptions,
): Promise<MachineStatus> {
    // What the plan expects this leg to find at arm time: BACKOFF and PARK
    // start ON the switch (they exist to retract off it); SEEK and LATCH start
    // clear. The node checks this against its own pin read and NAKs on
    // disagreement rather than running the leg under the wrong budget
    // semantics (docs/homing.md §1.4, §2.6).
    const intendedRetract = leg.kind === LegKind.BACKOFF || leg.kind === LegKind.PARK;
    const { armed, reason } = await home(
        link, leg.axis, leg.dir, intendedRetract,
        leg.startUs, leg.floorUs, leg.rampSteps, leg.maxSteps,
    );
    if (!armed) {
        throw new HomingError(leg, null,
            `${leg.axis}: Pico refused to arm the ${leg.kind} leg (${reason ?? "no reply"})`);
    }

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
    // Only `getstate` carries the per-axis `latched` mask — STATUS_RSP has no
    // room for it — and the mask is what distinguishes "this axis is on its
    // switch" from "some axis is". One extra round trip per leg, four per home.
    const st = await getState(link);
    opts.onLegDone?.(leg, st);

    if (st.alarm === AlarmReason.HOMING_FAIL) {
        // The budget wording is only honest for HomeFail.BUDGET. The other two
        // causes stop the leg wherever it happens to be, and quoting maxSteps at
        // them blames the switch for a bus dropout — which is exactly what it
        // did, reporting "within 211200 steps" for a leg that died at ~16000.
        if (st.homeFail === HomeFail.POLL) {
            throw new HomingError(leg, st,
                `${leg.axis} ${leg.kind}: node stopped answering mid-leg — the bus, ` +
                `not the axis. The move itself may have been fine; retry, and if it ` +
                `recurs at the same place look at wiring or termination`);
        }
        if (st.homeFail === HomeFail.DEADLINE) {
            throw new HomingError(leg, st,
                `${leg.axis} ${leg.kind}: still pulsing past the supervisor's timeout — ` +
                `the node's own ${leg.maxSteps}-step budget should have stopped it first`);
        }
        // Rotary. These never mean "ran out of budget", so they must be caught
        // before the fallthrough below quotes maxSteps at them -- the same
        // mistake the POLL/DEADLINE branches above exist to prevent. Each names
        // a different thing to go and look at, which is the only reason the
        // node distinguishes them at all.
        if (st.homeFail === HomeFail.INDEX_ABSENT) {
            throw new HomingError(leg, st,
                `${leg.axis} ${leg.kind}: the index sweep ran but never found the ` +
                `magnet — look at the Hall sensor, its magnet and its wiring, not at ` +
                `the budget. Run \`nodestat\` for the crossing count and the live ` +
                `hall/base readings`);
        }
        if (st.homeFail === HomeFail.INDEX_SHAPE) {
            throw new HomingError(leg, st,
                `${leg.axis} ${leg.kind}: the index feature no longer fits the node's ` +
                `capture window — the dip's shape has changed. Run \`hallscan\` on ` +
                `the node and compare it with the reference waveform`);
        }
        if (st.homeFail === HomeFail.INDEX_SLIP) {
            throw new HomingError(leg, st,
                `${leg.axis} ${leg.kind}: the index was found but did not repeat at a ` +
                `consistent interval — the axis slipped or stalled during the sweep. ` +
                `This is mechanical: check belt tension and driver current`);
        }
        // BUDGET, or firmware predating homefail= (undefined). The original
        // wording, which is correct for this case.
        //
        // How far it ACTUALLY went is no longer quoted here, because it is no
        // longer in this reply: the span moved to the node, where `nodestat`
        // reports it (commands.ts nodeStat). That is worth reading alongside
        // this message -- "never reached within 211200 steps" describes a switch
        // driven at and missed, and if the axis in fact stopped at 26000 the
        // budget was never the story.
        throw new HomingError(leg, st, leg.endsLatched
            ? `${leg.axis} ${leg.kind}: switch never reached within ${leg.maxSteps} steps` +
              ` — run \`nodestat\` for how far it actually went`
            : `${leg.axis} ${leg.kind}: never cleared the switch in ${leg.maxSteps} steps ` +
              `— back-off is likely shorter than the switch's release hysteresis`);
    }

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
            `but latched=0x${(st.axesLatched ?? 0).toString(16)} ` +
            `(alarm ${alarmName(st.alarm)})`);
    }
    return st;
}

/**
 * Run a full home: four legs, then the datum.
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
    for (let i = 0; i < plan.legs.length; i++) {
        const leg = plan.legs[i]!;
        opts.onLeg?.(leg, i, plan.legs.length);
        await runLeg(link, leg, opts);
    }
    if (!(await setOrigin(link, plan.axis, plan.datumSteps))) {
        throw new HomingError(null, null,
            `${plan.axis}: legs completed but setorigin ${plan.datumSteps} was refused ` +
            `— the axis is parked correctly but has no datum`);
    }
}
