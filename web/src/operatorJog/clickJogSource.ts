/**
 * operatorJog/clickJogSource.ts — open-session jog driven by tap/click intents.
 * Ported from host/ui/online/session.py _ClickJogSource.
 *
 * One click = one fixed distance (the button's mm value). Clicking again while
 * the machine is still moving ADDS that distance to what is left to travel, so
 * the motion extends instead of stopping and restarting — that is the blend.
 * Clicking the opposite direction cancels: the Pico ramps to rest from its
 * actual velocity and keeps position (via Link.abort, the §4.5 soft abort).
 * The session is open (docs/comms_architecture.md §2.3) because the TOTAL
 * distance depends on clicks that have not happened yet.
 *
 * Pacing: anchored against live telemetry (the `queuedUs` / `bufCount` from
 * the status sink, which keeps updating during transmission because the
 * reader never stopped reading). Between polls, the source extrapolates from
 * the last anchored sample — error is bounded to one poll interval rather than
 * accumulating. This is the D9/D12 observable-powering-open-sessions the
 * architecture was designed for.
 *
 * HOST-DECEL-FALLBACK: cancel() maps to `link.abort()` (the firmware's §4.5
 * soft-abort ramp). If the firmware does not have §4.5, the ramp-down
 * distance the host would need to plan lives at the inline comments below.
 * Start there.
 */

import type { PacketSource, StreamContext } from "../wire/link/session.js";
import type { Link } from "../wire/link/link.js";
import { packJog } from "../wire/format/packet.js";
import { microSegment } from "../wire/format/microsegment.js";
import type { AxisCalibration, ClickJogSourceOptions } from "./types.js";

const V_START = 50; // steps/s — rest velocity, matches make_jog
const LEAD_US = 250_000; // keep at most this much motion-time queued ahead (µs)
const LOW_WATER = 16; // segments — ~320 ms at CHUNK_MS, ~3 poll intervals
const CHUNK_MS = 20; // motion per emitted packet (ms)
const MAX_BURST = 16; // packets per pull()

function dt(): number {
    return typeof performance !== "undefined" ? performance.now() : Date.now();
}

export class ClickJogSource implements PacketSource {
    readonly axis: AxisCalibration;
    readonly axisIndex: number; // 0=x, 1=y, 2=z, 3=a
    readonly sign: number; // +1 or -1
    readonly rateMmPerSec: number;
    readonly feedSps: number;
    readonly accelSps2: number;
    readonly vStart: number;
    readonly leadUs: number;
    readonly lowWater: number;
    readonly chunkMs: number;
    readonly maxBurst: number;
    readonly link: Link | null;

    clicks = 1;
    emitted = 0; // packets emitted
    stepsTotal = 0; // steps committed to the wire

    private _remaining = 0; // steps still to travel
    private _v = V_START; // current velocity, ramped across chunks
    private _finished = false;
    private _cancelled = false; // reversal/stop — the Pico is ramping
    private _resolveWake: (() => void) | null = null;

    // Anchored pacing: the report ANCHORS, the clock INTERPOLATES.
    private _anchorUs = 0; // queuedUs as of the last sample we used
    private _anchorT: number | null = null; // monotonic() when that sample landed
    private _anchorStamp = -1; // which sample it was
    private _sinceAnchorS = 0; // motion-seconds emitted since then

    /**
     * @param link — may be null in a testing/snapshot context. When null, cancel
     *   does not call link.abort().
     */
    constructor(
        axis: AxisCalibration,
        axisLetter: "x" | "y" | "z" | "a",
        sign: number,
        rateMmPerSec: number,
        link: Link | null,
        opts: ClickJogSourceOptions = {},
    ) {
        this.axis = axis;
        this.axisIndex = { x: 0, y: 1, z: 2, a: 3 }[axisLetter];
        this.sign = sign;
        this.rateMmPerSec = rateMmPerSec;
        this.link = link;
        this.vStart = opts.vStart ?? V_START;
        this.leadUs = opts.leadUs ?? LEAD_US;
        this.lowWater = opts.lowWater ?? LOW_WATER;
        this.chunkMs = opts.chunkMs ?? CHUNK_MS;
        this.maxBurst = opts.maxBurst ?? MAX_BURST;
        this.feedSps = Math.max(1, rateMmPerSec * axis.stepsPerUnit);
        this.accelSps2 = Math.max(rateMmPerSec * 8, 50) * axis.stepsPerUnit;
    }

    /** Another click in the same direction — extend the move. Returns false if
     *  this source has already finished, so the caller starts a new one. */
    add(steps: number): boolean {
        if (this._finished) return false;
        this._remaining += steps;
        this.clicks++;
        this._wake();
        return true;
    }

    /** Reversal or stop — hand the deceleration to the Pico (§4.5). */
    cancel(): void {
        this._remaining = 0;
        this._cancelled = true;
        this._wake();
        if (this.link) this.link.abort();
        // HOST-DECEL-FALLBACK: if §4.5 is absent, the host must plan its own
        // deceleration here. Compute the ramp-down distance from the current
        // estimated velocity via _decelDistance(), append decel packets to
        // the window (as ListSource packets — a closed traling burst after
        // truncation), and skip calling link.abort().
    }

    // -- PacketSource --------------------------------------------------------

    async pull(ctx: StreamContext): Promise<Uint8Array[] | null> {
        // Re-check under the call: finished since last poll? Cancelled since?
        if (this._finished) return null;
        if (this._cancelled) {
            // The Pico is ramping and has thrown away the ring. Nothing we
            // emit now would be accepted (NACK_ABORTING), and nothing we
            // already sent survives. The session is simply over.
            this._finished = true;
            return null;
        }

        let remaining = this._remaining;

        if (remaining <= 0) {
            // Distance spent — but do NOT finish while the machine is still
            // executing what we already sent. Staying open is what lets a
            // click arriving mid-move blend into it instead of starting a
            // fresh session.
            if (!this._draining(ctx)) {
                this._finished = true;
                return null;
            }
            await this._wait(10); // a click wakes this immediately
            return [];
        }

        // Pace on the anchored estimate (see _leadUs). bufCount is the hard
        // ceiling — the ring is finite regardless of what any time-based
        // measure claims.
        const buf = ctx.bufCount;
        if (this._leadUs(ctx) >= this.leadUs || (buf !== undefined && buf >= this.lowWater)) {
            await this._wait(5);
            return []; // nothing right now, still open
        }

        // Fill UP TO the lead target in one call, rather than one chunk per
        // pull(). Emitting a single CHUNK_MS packet per pull ties throughput
        // to pull cadence; a pull that returns [] costs ~25 ms while one
        // packet only buys CHUNK_MS = 20 ms of motion. The source falls
        // behind.
        const batch: Uint8Array[] = [];
        const dtSec = this.chunkMs / 1000;

        while (
            batch.length < this.maxBurst &&
            remaining > 0 &&
            this._leadUs(ctx) < this.leadUs
        ) {
            // Decelerate once the distance left is only enough to stop.
            const targetV =
                remaining <= this._decelDistance(this._v) ? this.vStart : this.feedSps;

            const v0 = this._v;
            const v1 =
                targetV > v0
                    ? Math.min(targetV, v0 + this.accelSps2 * dtSec)
                    : Math.max(targetV, v0 - this.accelSps2 * dtSec);
            const vAvg = Math.max((v0 + v1) / 2, 1);

            const steps = Math.max(1, Math.min(Math.round(vAvg * dtSec), Math.round(remaining)));

            this._v = v1;
            this._remaining = Math.max(0, this._remaining - steps);
            remaining = this._remaining;
            this._sinceAnchorS += steps / vAvg;
            this.emitted++;

            const inv = this.axis.invert ? -1 : 1;
            const vec: [number, number, number, number] = [0, 0, 0, 0];
            vec[this.axisIndex] = Math.trunc(inv * this.sign * steps);

            const interval = Math.max(
                1,
                Math.min(Math.trunc(150_000_000 / Math.max(vAvg, 1)), 150_000_000),
            );
            // The fCpu is hardcoded at 150 MHz (the Sim default + a common Pico
            // clock). Callers that need a different value should override via
            // the opts or compose their own source.
            // FUTURE: pull fCpu from a config field the opts accept.
            const flags = 0; // no PATH_END on individual jog chunks (the last
            // click's last chunk carries no marker; the session ends when the
            // machine drains back to rest).

            this.stepsTotal += steps;
            batch.push(
                packJog(
                    microSegment(vec[0], vec[1], vec[2], vec[3], interval, flags),
                ),
            );
        }

        return batch;
    }

    // -- pacing -------------------------------------------------------------

    /** Motion time queued ahead of the machine, in microseconds. */
    private _leadUs(ctx: StreamContext): number {
        const [queued, stamp, at] = ctx.queuedSample;
        if (queued !== undefined && stamp !== this._anchorStamp) {
            this._anchorStamp = stamp;
            this._anchorUs = queued;
            this._anchorT = at;
            this._sinceAnchorS = 0;
        }
        if (this._anchorT === null) {
            return Math.max(0, this._sinceAnchorS * 1e6);
        }
        const elapsedUs = (dt() - this._anchorT) * 1e3; // ms → µs
        return Math.max(0, this._anchorUs + this._sinceAnchorS * 1e6 - elapsedUs);
    }

    /** Is the machine still executing what we already sent? */
    private _draining(ctx: StreamContext): boolean {
        const q = ctx.queuedUs;
        if (q === undefined) {
            const b = ctx.bufCount;
            return (b !== undefined && b > 0) || this._leadUs(ctx) > 0;
        }
        return q > 0 || this._leadUs(ctx) > 0;
    }

    /** Steps needed to get from v back down to V_START. */
    private _decelDistance(v: number): number {
        return Math.max(0, (v * v - this.vStart * this.vStart) / (2 * this.accelSps2));
    }

    // -- intent queue -------------------------------------------------------

    private _wake(): void {
        if (this._resolveWake) {
            const r = this._resolveWake;
            this._resolveWake = null;
            r();
        }
    }

    /** A promise that resolves on the next click intent or timeout. */
    private _wait(timeoutMs: number): Promise<void> {
        this._wake(); // resolve any stale waiter
        return new Promise<void>((r) => {
            this._resolveWake = r;
            setTimeout(() => {
                if (this._resolveWake === r) {
                    this._resolveWake = null;
                    r();
                }
            }, timeoutMs);
        });
    }
}