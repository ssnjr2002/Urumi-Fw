/**
 * link/link.ts — owns a Transport, one Demux, one Writer, and the sink set.
 * Ported from host/protocol/link.py `Link` (minus the in-tree backends, which
 * live in link/backends/).
 *
 * Concerns SUBSCRIBE rather than seize (D1/D3): command(), getStatus() and
 * session() can all be in flight at once, because each awaits its own sink
 * while the single read loop keeps draining the Transport into the Demux.
 * Nothing else reads the Transport; the read loop is the reader (D1).
 *
 * Construct `new Link(transport)` over any Transport (the in-process Sim, a
 * browser WebSerial port, or a Node serialport). The Link is environment-blind
 * — only the backend knows what kind of pipe it is.
 */

import type { Transport, Writable } from "./transport.js";
import { Writer } from "./writer.js";
import { Demux, makeSinks, type DemuxSinks } from "./demux.js";
import type { Sink, LatestSink } from "./sink.js";
import { Ack, Nack } from "./demux.js";
import { ListSource, DEFAULT_WINDOW, Session, fatalReasonName, type StreamResult } from "./session.js";
import type { PacketSource, StreamContext } from "./session.js";

/**
 * Optional backend hook: a Transport may expose `attach(demux)` so the Sim
 * (which feeds replies straight into the demux via `write`) can reference it.
 * Real-port backends (WebSerial, Node) ignore this — they pump bytes through
 * `read()`, which the Link's own read loop drains into the demux.
 */
export interface Attachable {
    attach(demux: Demux): void;
}
import {
    MAGIC_ABORT,
    MAGIC_SEQRESET,
    MAGIC_STATUS_REQ,
} from "../format/constants.js";
import { parseStatusRsp, type MachineStatus } from "../format/status.js";

export class Link {
    readonly sinks: DemuxSinks;
    readonly demux: Demux;
    readonly writer: Writer;
    private readonly _transport: Transport;
    private _textLock: Promise<unknown> = Promise.resolve();
    private readonly _readLoop: Promise<void>;

    /**
     * Orphaned text lines discarded by command(). Should stay 0 — anything
     * else means a command replied with more lines than it is allowed to, a
     * firmware contract bug worth surfacing rather than silently absorbing.
     */
    textDesyncs = 0;
    private _closed = false;
    /**
     * When true, sessions created via stream()/session() log each ACK, NACK,
     * go-back and fatal at console.debug level (D17). Default off — flip on at
     * connect time to trace a silent stream failure (the bare-`false` return
     * problem the orchestrate demo hit before this fix).
     */
    verbose = false;

    constructor(transport: Transport) {
        this._transport = transport;
        this.sinks = makeSinks();
        this.demux = new Demux(this.sinks);
        this.writer = new Writer(transport as Writable);
        // A Sim-style backend feeds replies straight into the demux via its
        // own `write`; real-port backends pump bytes through `read()` which
        // the _pump loop drains into the same demux. Either way, hand the
        // demux to the backend if it wants it.
        const attachable = transport as Transport & Partial<Attachable>;
        if (typeof attachable.attach === "function") {
            attachable.attach(this.demux);
        }
        this._readLoop = this._pump();
    }

    /** Construct over any Transport; convenience alias for `new Link(transport)`. */
    static open(transport: Transport): Link {
        return new Link(transport);
    }

    get closed(): boolean {
        return this._closed;
    }

    /** Underlying transport (for backends that need raw access). */
    get transport(): Transport {
        return this._transport;
    }

    private async _pump(): Promise<void> {
        try {
            for await (const chunk of this._transport.read()) {
                this.demux.feed(chunk);
            }
        } catch {
            // read failed (port closed / unplugged) — nothing to do here; a
            // pending command/session resolves on its sink timeout, and the
            // next call sees `closed`.
        }
        this._closed = true;
    }

    // -- control plane --------------------------------------------------------

    /**
     * Send one control-plane line and return the reply line (stripped), or ""
     * on timeout. No flush of the PORT beforehand: routing on magic means a
     * stale status reply or a stream ACK cannot land in the text sink (D10).
     * Text stays strictly one-outstanding (D11): the next line in the sink is
     * ours.
     *
     * The text sink itself is drained first — under the one-outstanding lock,
     * any line already sitting there is orphaned (its awaiter timed out, or
     * the Pico emitted more lines than the contract allows). Leaving orphans
     * would make every later command read the previous one's tail, so a single
     * contract breach desyncs the plane permanently rather than transiently.
     * A non-zero drain increments textDesyncs so the breach is loud.
     */
    async command(text: string, timeoutMs = 1000): Promise<string> {
        const run = this._textLock.then(async () => {
            const stale = this.sinks.text.clear();
            if (stale > 0) this.textDesyncs += stale;
            await this.writer.writeText(text);
            return (await this.sinks.text.get(timeoutMs)) ?? "";
        });
        this._textLock = run.then(
            () => undefined,
            () => undefined,
        );
        return run as Promise<string>;
    }

    /**
     * Fire-and-forget control command — needs the writer, not a reply slot.
     * `stop` is the case that matters: estop must never queue behind a pending
     * text command, and it correlates nothing (confirmation arrives on the
     * status sink as the state goes ESTOP→ALARM).
     */
    send(text: string): Promise<void> {
        return this.writer.writeText(text);
    }

    /**
     * Binary mirror of command("getstate") AND command("getpos") — one byte
     * out, one frame back. Cheap enough to poll during a stream, since it
     * slots into a boundary between MSEG packets instead of needing a whole
     * ASCII line. Coherent: state, position, expectedSeq and queuedUs describe
     * ONE instant on the Pico.
     */
    async getStatus(timeoutMs = 1000): Promise<MachineStatus> {
        const before = this.sinks.status.sample[1];
        await this.writer.writeFrame(new Uint8Array([MAGIC_STATUS_REQ]));
        const [data] = await this.sinks.status.waitUpdate(timeoutMs, before);
        if (!data) throw new Error("no STATUS_RSP within timeout");
        return parseStatusRsp(data);
    }

    /** Latest status sample without a round trip, or null. */
    get status(): MachineStatus | null {
        const raw = this.sinks.status.value;
        return raw ? parseStatusRsp(raw) : null;
    }

    /**
     * Soft abort (§4.5): ramp to rest, flush the ring, land IDLE with position
     * intact. Fire-and-forget like `stop` — it correlates nothing, so it takes
     * the writer lock but no reply slot and can never queue behind a pending
     * text command. Confirmation arrives on the status sink as the state
     * settles. Packets sent after this get NACK_ABORTING until the machine
     * reaches IDLE; the session handles that as a barrier, not an error.
     */
    abort(): void {
        void this.writer.writeFrame(new Uint8Array([MAGIC_ABORT]));
    }

    /**
     * Align the Pico's expectedSeq with a session's fresh seq counter. Every
     * session stamps from 0, so this must precede one. Binary (§4.3): one byte
     * out, ACK(0) back on the ack sink. The ACK is drained HERE rather than
     * left for the session — a session opening on a stale ACK would advance
     * its window against a packet it never sent.
     */
    async resetSeq(timeoutMs = 1000): Promise<boolean> {
        this.sinks.ack.clear();
        await this.writer.writeFrame(new Uint8Array([MAGIC_SEQRESET]));
        const r = await this.sinks.ack.get(timeoutMs);
        return r !== null;
    }

    /**
     * Stream a closed sequence (a job) with Go-Back-N. Resets the seq first.
     * Returns the Stream outcome (D17): `ok` is the boolean legacy callers
     * tested (`if (await link.stream(pkts)) …` still works — `ok` is truthy),
     * and on failure `fatalReason` names the cause. Stats are the tally at exit
     * so a failed stream shows how far it got.
     */
    async stream(packets: Iterable<Uint8Array>, window: number = DEFAULT_WINDOW): Promise<StreamResult> {
        await this.resetSeq();
        const sess = this.session(new ListSource([...packets]), window);
        await sess.run();
        return sess.result();
    }

    /**
     * Build a Session over any PacketSource — use this directly for an OPEN
     * session (manual jogging), where packets are produced in response to
     * operator input and the session ends by truncation rather than exhaustion.
     * Caller must resetSeq() first; stream() does it for you. The session
     * inherits `verbose` from this Link (D17).
     */
    session(source: PacketSource, window: number = DEFAULT_WINDOW): Session {
        const sess = new Session(this.writer, this.sinks.ack, source, this.sinks.status, window);
        sess.verbose = this.verbose;
        return sess;
    }

    async close(): Promise<void> {
        await this._transport.close();
    }
}

// Re-export the sink/frame types a caller of Link commonly needs.
// `fatalReasonName` is a value (not a type) so it leaves the type-only list.
export type { Sink, LatestSink, Ack, Nack, PacketSource, Session, StreamContext, StreamResult };
export { fatalReasonName };