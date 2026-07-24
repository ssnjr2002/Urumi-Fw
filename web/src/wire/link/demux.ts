/**
 * link/demux.ts — the demultiplexing state machine (D1–D5).
 * Ported from host/protocol/reader.py (Demux, Ack, Nack, CfgReply, make_sinks).
 *
 * One reader owns the port for the connection's lifetime, classifies every
 * inbound byte, and fans whole frames out to typed sinks. Nothing else reads
 * the port; sessions and pollers SUBSCRIBE rather than seize. That is what
 * allows a status poll to be in flight while a stream is writing.
 *
 * MUST BE A STATE MACHINE, NOT A MAGIC SCANNER. Not every inbound byte is
 * framing: a CFG_DATA payload is an opaque msgpack blob that can contain 0xAA,
 * 0xBB or 0xA6 at any offset. A scanner would emit a phantom ACK from inside
 * config data, advancing a session's `base` against a packet that was never
 * sent — blob-dependent, intermittent, near-impossible to trace. So
 * fixed-length frames are consumed BLIND by count, length-prefixed payloads are
 * consumed OPAQUELY by count, and nothing is ever scanned for structure.
 *
 * Pure: no I/O. `feed(bytes) -> dispatch to sinks`. The backend's read loop
 * pumps chunks in; frame boundaries need not align with chunk boundaries. All
 * four sinks are required — a missing sink is a routing hole, not a default.
 */

import { Sink, LatestSink } from "./sink.js";
import {
    MAGIC_ACK,
    MAGIC_NACK,
    MAGIC_STATUS_RSP,
    STATUS_RSP_SIZE,
    MAGIC_CFG_RDY,
    MAGIC_CFG_ACK,
    MAGIC_CFG_NACK,
    MAGIC_CFG_DATA,
} from "../format/constants.js";
import { CFG_DATA_HDR_SIZE, MAX_CFG_PAYLOAD, unpackCfgDataHeader } from "../format/cfg.js";

// ── frame types: routing objects, not parsed payloads ─────────────────────────
// The demux's job is routing, not interpretation. Status parsing stays in
// format/status.ts; the demux hands the raw 30 bytes to the status sink.

/** Cumulative ACK — `expectedSeq` is the Pico's next-wanted wire seq. */
export class Ack {
    constructor(readonly expectedSeq: number) {}
}

/** Stream NACK — `reason` is one of the NACK_* constants. */
export class Nack {
    constructor(readonly reason: number) {}
}

/** Config reply. `reason` is set for CFG_NACK; `payload` and `crc32` for CFG_DATA. */
export class CfgReply {
    constructor(
        readonly kind: number,
        readonly reason?: number,
        readonly payload?: Uint8Array,
        readonly crc32?: number,
    ) {}
}

// ── the standard sink set ──────────────────────────────────────────────────────

export interface DemuxSinks {
    /** ack/nack — ordered, never drops. A Session subscribes for its span. */
    ack: Sink<Ack | Nack>;
    /** status — latest-wins. StatusMonitor + open jog sessions read this. */
    status: LatestSink<Uint8Array>;
    /** text — ordered, never drops. The one-outstanding control plane. */
    text: Sink<string>;
    /** cfg — the config transaction sink. */
    cfg: Sink<CfgReply>;
}

export function makeSinks(): DemuxSinks {
    return {
        ack: new Sink<Ack | Nack>("ack"),
        status: new LatestSink<Uint8Array>("status"),
        text: new Sink<string>("text"),
        cfg: new Sink<CfgReply>("cfg"),
    };
}

// ── states ────────────────────────────────────────────────────────────────────

const S_IDLE = 0; // between frames, classifying the next magic
const S_FIXED = 1; // accumulating a known-length frame
const S_CFG_DATA_HDR = 2; // accumulating CFG_DATA's 9-byte header
const S_CFG_DATA_PAY = 3; // accumulating `length` opaque payload bytes
const S_TEXT = 4; // accumulating an ASCII line to \n

// Inbound fixed-length frames: magic -> total frame size including the magic.
const FIXED_SIZES: Readonly<Record<number, number>> = {
    [MAGIC_ACK]: 3,
    [MAGIC_NACK]: 3,
    [MAGIC_STATUS_RSP]: STATUS_RSP_SIZE,
    [MAGIC_CFG_RDY]: 1, // complete on its own
    [MAGIC_CFG_ACK]: 1, // complete on its own
    [MAGIC_CFG_NACK]: 2,
};

const MAX_TEXT_LINE = 512; // guard: a runaway line must not grow without bound

// ── the state machine ──────────────────────────────────────────────────────────

export interface DemuxStats {
    readonly frames: number;
    readonly textLines: number;
    readonly unknownBytes: number;
    readonly overruns: number;
}

export class Demux {
    readonly ack: Sink<Ack | Nack>;
    readonly status: LatestSink<Uint8Array>;
    readonly text: Sink<string>;
    readonly cfg: Sink<CfgReply>;

    private _state: number = S_IDLE;
    private _buf: number[] = [];
    private _need = 0;
    private _magic = 0;
    private _cfgCrc = 0;

    // Diagnostics. `unknownBytes` is the one to watch: a nonzero count means a
    // firmware/host version mismatch or a genuine desync — the only way either
    // becomes visible (D5).
    unknownBytes = 0;
    frames = 0;
    textLines = 0;
    overruns = 0;

    constructor(sinks: DemuxSinks) {
        this.ack = sinks.ack;
        this.status = sinks.status;
        this.text = sinks.text;
        this.cfg = sinks.cfg;
    }

    /**
     * Consume a chunk. Frame boundaries need not align with chunk boundaries
     * — a frame split across two reads is the normal case at the 64-byte USB
     * packet quantum, so this is exercised constantly, not rarely.
     */
    feed(data: Uint8Array): void {
        for (let i = 0; i < data.length; i++) {
            this._byte(data[i]!);
        }
    }

    /** True when not mid-frame. Tests assert this after a complete sequence. */
    get idle(): boolean {
        return this._state === S_IDLE;
    }

    stats(): DemuxStats {
        return {
            frames: this.frames,
            textLines: this.textLines,
            unknownBytes: this.unknownBytes,
            overruns: this.overruns,
        };
    }

    private _byte(b: number): void {
        if (this._state === S_IDLE) {
            this._dispatch(b);
            return;
        }

        if (this._state === S_TEXT) {
            if (b === 0x0a) {
                // \n terminates
                const line = bytesToAscii(this._buf);
                this._buf = [];
                this._state = S_IDLE;
                if (line.length > 0) {
                    this.textLines++;
                    this.text.put(line);
                }
            } else if (this._buf.length >= MAX_TEXT_LINE) {
                this.overruns++;
                this._buf = [];
                this._state = S_IDLE;
            } else {
                this._buf.push(b);
            }
            return;
        }

        // All remaining states are pure byte counting — no inspection of content.
        this._buf.push(b);
        if (this._buf.length < this._need) return;

        if (this._state === S_FIXED) this._emitFixed();
        else if (this._state === S_CFG_DATA_HDR) this._cfgDataHeader();
        else this._emitCfgData();
    }

    // -- idle dispatch --------------------------------------------------------

    private _dispatch(b: number): void {
        const size = FIXED_SIZES[b];
        if (size !== undefined) {
            this._magic = b;
            if (size === 1) {
                // complete on its own — CFG_RDY or CFG_ACK
                this.frames++;
                this.cfg.put(new CfgReply(b));
                return;
            }
            this._buf = [b];
            this._need = size;
            this._state = S_FIXED;
            return;
        }

        if (b === MAGIC_CFG_DATA) {
            this._buf = [b];
            this._need = CFG_DATA_HDR_SIZE;
            this._state = S_CFG_DATA_HDR;
            return;
        }

        if (b < 0x80) {
            // ASCII — control-plane text. bit 7 clear, so no collision with magics.
            this._buf = [];
            if (b !== 0x0a) {
                // a bare \n is not a line
                this._buf.push(b);
                this._state = S_TEXT;
            }
            return;
        }

        // Bit 7 set but not a magic we know: discard exactly one byte and stay
        // idle, so the next byte gets a fresh classification. Discarding more
        // would risk eating the start of a valid frame (D5).
        this.unknownBytes++;
    }

    // -- completions ----------------------------------------------------------

    private _emitFixed(): void {
        const buf = this._buf;
        const magic = this._magic;
        this._state = S_IDLE;
        this.frames++;

        if (magic === MAGIC_ACK) {
            this.ack.put(new Ack(buf[1]!));
        } else if (magic === MAGIC_NACK) {
            this.ack.put(new Nack(buf[1]!));
        } else if (magic === MAGIC_STATUS_RSP) {
            this.status.put(Uint8Array.from(buf));
        } else if (magic === MAGIC_CFG_NACK) {
            this.cfg.put(new CfgReply(magic, buf[1]!));
        }
        this._buf = [];
    }

    private _cfgDataHeader(): void {
        const hdr = unpackCfgDataHeader(Uint8Array.from(this._buf));
        this._cfgCrc = hdr.crc32;

        if (hdr.length === 0) {
            // no stored config — done
            this._state = S_IDLE;
            this.frames++;
            this._buf = [];
            this.cfg.put(new CfgReply(MAGIC_CFG_DATA, undefined, new Uint8Array(0), hdr.crc32));
            return;
        }

        if (hdr.length > MAX_CFG_PAYLOAD) {
            // Do not enter the payload state: we would consume `length` bytes of
            // whatever follows, blinding every other plane. Drop and resync.
            this.overruns++;
            this._state = S_IDLE;
            this._buf = [];
            return;
        }

        this._buf = [];
        this._need = hdr.length;
        this._state = S_CFG_DATA_PAY;
    }

    private _emitCfgData(): void {
        const payload = Uint8Array.from(this._buf);
        this._buf = [];
        this._state = S_IDLE;
        this.frames++;
        this.cfg.put(new CfgReply(MAGIC_CFG_DATA, undefined, payload, this._cfgCrc));
    }
}

/** Decode the text buffer: ASCII with U+FFFD replacement, trailing \r stripped. */
function bytesToAscii(buf: number[]): string {
    let s = "";
    for (const b of buf) {
        s += b < 0x80 ? String.fromCharCode(b) : "\uFFFD";
    }
    return s.replace(/\r+$/, "");
}