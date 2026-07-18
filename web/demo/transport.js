/**
 * transport.js — WebSerial wrapper for the RP2350 wire protocol (Phase 1).
 *
 * Protocol: docs/wire_protocol.md
 *   Data plane  — binary magic-dispatched: MSEG (0xAB), JOG (0xAE), ACK (0xAA),
 *                 NACK (0xBB), STATUS_REQ (0xA5), STATUS_RSP (0xA6)
 *   Control plane — text lines (\n-terminated): ping, enable, disable, seqreset,
 *                   getstate, getpos, setorigin, pause, resume, cancel, stop
 *
 * Streaming: Go-Back-N with window=16, cumulative ACKs.
 *   \nseqreset\n + MCFG preamble before each MSEG stream.
 *   ACK carries expectedSeq (cumulative accept point) → advance window to it.
 *   NACK_FULL → back off 50 ms, drain stale responses, rewind.
 *   NACK_CRC  → settle 5 ms, drain, rewind.
 *   NACK_BAD_MAGIC → fatal throw.
 */

import { packMicrosegment, crc8, PACKET_SIZE } from '../src/wire/packet.js';

// ── wire constants ─────────────────────────────────────────────────────────────

const JOG_MAGIC  = 0xAE;
const ACK_MAGIC  = 0xAA;
const NACK_MAGIC = 0xBB;
export const STATUS_REQ = 0xA5;
export const STATUS_RSP_MAGIC = 0xA6;

export const NACK_CRC       = 0x01;
export const NACK_FULL      = 0x02;
export const NACK_BAD_MAGIC = 0x03;
export const NACK_PAUSED    = 0x04;
export const NACK_BAD_STATE = 0x06;

export const STATE_IDLE    = 0;
export const STATE_RUNNING = 1;
export const STATE_ESTOP   = 2;
export const STATE_ALARM   = 3;
export const STATE_PAUSED  = 4;
export const STATE_HOMING  = 5;

const WINDOW = 16;
const BAUD_RATE = 115200;

// ── packet builders ────────────────────────────────────────────────────────────

/** Build the 6-byte Phase 1 MCFG job stream preamble. */
export function buildMcfg(requiredAxes) {
    // "MCFG" magic (4 bytes LE) + version 1 + required_axes bitmask
    return new Uint8Array([0x4D, 0x43, 0x46, 0x47, 0x01, requiredAxes & 0x0F]);
}

/**
 * Pack a MicroSegment as a JOG packet (magic 0xAE, same 26-byte layout as MSEG).
 * Used for operator jogs in IDLE / PAUSED state — not for in-job streaming.
 */
export function packJog(seg, seq = 0) {
    const buf = new ArrayBuffer(PACKET_SIZE);
    const dv  = new DataView(buf);
    const u8  = new Uint8Array(buf);
    dv.setUint8(0,  JOG_MAGIC);
    dv.setInt32(1,  seg.dx,       true);
    dv.setInt32(5,  seg.dy,       true);
    dv.setInt32(9,  seg.dz,       true);
    dv.setInt32(13, seg.da,       true);
    dv.setUint32(17, seg.interval, true);
    dv.setUint8(21, seg.flags & 0xFF);
    dv.setUint8(22, seq & 0xFF);
    // [23..24] pad — zeroed by ArrayBuffer init
    dv.setUint8(25, crc8(u8, 0, PACKET_SIZE - 1));
    return u8;
}

// ── buffered byte reader ───────────────────────────────────────────────────────

/**
 * ByteReader wraps a WebSerial ReadableStream and allows byte-at-a-time reads
 * regardless of how the browser chunks incoming data.
 */
class ByteReader {
    constructor(readable) {
        this._queue   = [];
        this._resolve = null;

        // Pump the readable into the queue on a background async loop.
        const reader = readable.getReader();
        const self = this;
        (async () => {
            try {
                for (;;) {
                    const { value, done } = await reader.read();
                    if (done) break;
                    for (const b of value) {
                        self._queue.push(b);
                        if (self._resolve) {
                            const r = self._resolve;
                            self._resolve = null;
                            r();
                        }
                    }
                }
            } catch { /* port closed */ }
        })();
    }

    async readByte() {
        if (this._queue.length > 0) return this._queue.shift();
        await new Promise(r => { this._resolve = r; });
        return this._queue.shift();
    }

    async readBytes(n) {
        const out = new Uint8Array(n);
        for (let i = 0; i < n; i++) out[i] = await this.readByte();
        return out;
    }

    /**
     * Read one response: ACK (3B), NACK (3B), STATUS_RSP (9B), or text line.
     *
     * STATUS_RSP layout (docs/wire_protocol.md + packets.py STATUS_RSP_SIZE=9):
     *   [0xA6][state][enabled][homed][alarm][running][buf_count lo][buf_count hi][CRC8]
     *
     * Returns a typed object:
     *   { type: 'ack',    ackSeq }   ackSeq = expectedSeq, cumulative accept point
     *   { type: 'nack',   reason }
     *   { type: 'status', machineState, axesEnabled, axesHomed, alarmReason,
     *                     runningReason, bufCount }
     *   { type: 'text',   line }
     */
    async readResponse() {
        const first = await this.readByte();

        if (first === ACK_MAGIC) {
            // rest[0] = expectedSeq (cumulative ACK point); rest[1] reserved.
            const rest = await this.readBytes(2);
            return { type: 'ack', ackSeq: rest[0], seqLo: rest[0], seqHi: rest[1] };
        }

        if (first === NACK_MAGIC) {
            const rest = await this.readBytes(2);
            return { type: 'nack', reason: rest[0] };
        }

        if (first === STATUS_RSP_MAGIC) {
            // 8 more bytes: state, enabled, homed, alarm, running, buf_count(u16 LE), CRC8
            const rest = await this.readBytes(8);
            return {
                type:          'status',
                machineState:  rest[0],
                axesEnabled:   rest[1],
                axesHomed:     rest[2],
                alarmReason:   rest[3],
                runningReason: rest[4],
                bufCount:      rest[5] | (rest[6] << 8),
                // rest[7] is CRC8 — consumed but not verified
            };
        }

        // Text line — first byte is ASCII (bit 7 = 0)
        let s = String.fromCharCode(first);
        for (;;) {
            const b = await this.readByte();
            if (b === 0x0A) return { type: 'text', line: s.trim() };
            s += String.fromCharCode(b);
        }
    }
}

// ── SerialTransport ────────────────────────────────────────────────────────────

export class SerialTransport {
    constructor() {
        this._port   = null;
        this._writer = null;
        this._reader = null;
        // Serializes every request (sendText/getPosition/pollStatus/sendStream/
        // sendJogBurst) onto one chain. The ByteReader has a single waiter slot,
        // so two concurrent readResponse() calls would clobber each other's
        // resolver and deadlock — e.g. a background status poll colliding with a
        // getpos on re-run. All public entry points funnel through _serialize().
        this._lock = Promise.resolve();
    }

    get connected() { return this._port !== null; }

    /** Run `fn` after all previously-queued requests settle (FIFO, exclusive). */
    _serialize(fn) {
        const run = this._lock.then(fn, fn);
        this._lock = run.then(() => {}, () => {}); // keep the chain alive on error
        return run;
    }

    /** Open a WebSerial port (shows browser port-picker). */
    async connect() {
        this._port = await navigator.serial.requestPort();
        await this._port.open({ baudRate: BAUD_RATE });
        this._writer = this._port.writable.getWriter();
        this._reader = new ByteReader(this._port.readable);
    }

    async disconnect() {
        try {
            this._writer?.releaseLock();
            await this._port?.close();
        } catch { /* ignore */ }
        this._port   = null;
        this._writer = null;
        this._reader = null;
    }

    // ── control plane ────────────────────────────────────────────────────────

    /**
     * Send a text command (with automatic \n) and return the reply line.
     */
    sendText(cmd) { return this._serialize(() => this._sendText(cmd)); }

    async _sendText(cmd) {
        const line = cmd.endsWith('\n') ? cmd : cmd + '\n';
        const stale = [...this._reader._queue];
        if (stale.length > 0) {
            console.warn(`[serial] ${stale.length} stale bytes in queue before "${cmd}": ${stale.map(b => '0x' + b.toString(16)).join(' ')}`);
            this._reader._queue.length = 0;
        }
        console.log(`[serial tx] text: ${JSON.stringify(line.trim())}`);
        await this._writer.write(new TextEncoder().encode('\n' + line));
        const resp = await this._reader.readResponse();
        console.log(`[serial rx] text reply:`, resp);
        return resp.type === 'text' ? resp.line : '';
    }

    /**
     * Query the machine's tracked step position via the `getpos` control
     * command. The Pico replies with a text line "pos X Y Z A".
     *
     * The values are per-axis EMITTED (post-invert) steps — exactly the
     * machinePos[] the firmware accumulates from the wire deltas. Callers that
     * need TRUE (pre-invert) step space must un-invert per axis themselves.
     *
     * Returns { x, y, z, a }.
     */
    getPosition() { return this._serialize(() => this._getPosition()); }

    async _getPosition() {
        const line = await this._sendText('getpos');
        const nums = line.match(/-?\d+/g);
        if (!nums || nums.length < 4) throw new Error(`bad getpos reply: "${line}"`);
        return { x: +nums[0], y: +nums[1], z: +nums[2], a: +nums[3] };
    }

    /**
     * Send STATUS_REQ (0xA5) and return the STATUS_RSP fields.
     * Skips any text lines or unexpected responses until STATUS_RSP arrives.
     */
    pollStatus() { return this._serialize(() => this._pollStatus()); }

    async _pollStatus() {
        await this._writer.write(new Uint8Array([STATUS_REQ]));
        for (;;) {
            const resp = await this._reader.readResponse();
            if (resp.type === 'status') {
                console.log(`[serial rx] status: state=${resp.machineState} buf=${resp.bufCount}`);
                return resp;
            }
            console.log(`[serial rx] unexpected during pollStatus:`, resp);
        }
    }

    // ── data plane ───────────────────────────────────────────────────────────

    /**
     * Stream MicroSegments as MSEG packets (0xAB) using Go-Back-N (window=16).
     *
     * Protocol handshake before sending:
     *   1. seqreset  — zeroes the Pico's expectedSeq so seq 0 lines up
     *   2. MCFG (6B) — required_axes preamble
     *   3. MSEG packets — Go-Back-N window=16
     *
     * @param {MicroSegment[]} segments
     * @param {number}         requiredAxes  bit0=X bit1=Y bit2=Z bit3=A
     * @param {function}       onProgress    (sent, total) callback
     */
    sendStream(segments, requiredAxes, onProgress) {
        return this._serialize(() => this._sendStream(segments, requiredAxes, onProgress));
    }

    async _sendStream(segments, requiredAxes, onProgress) {
        // Leading \n flushes any partial line on the Pico before seqreset (mirrors
        // Python stream.py: ser.write(b"\nseqreset\n")).
        const n = segments.length;
        console.log(`[serial tx] seqreset (${n} segments, axes=0x${requiredAxes.toString(16)})`);
        await this._writer.write(new TextEncoder().encode('\nseqreset\n'));
        const seqReply = await this._reader.readResponse();
        console.log(`[serial rx] seqreset reply:`, seqReply);
        // await this._writer.write(buildMcfg(requiredAxes)); // dont implement now, firmware does NOT handle this right now. will cause bugs if left in
        if (n === 0) return;

        // Pre-pack all packets with rolling seq numbers (uint8).
        const pkts = segments.map((seg, i) => packMicrosegment(seg, i & 0xFF));

        let base    = 0; // index of oldest unacknowledged packet
        let nextIdx = 0; // index of next packet to send

        while (base < n) {
            // Fill the window: send until full or end of segments.
            while (nextIdx < n && nextIdx - base < WINDOW) {
                await this._writer.write(pkts[nextIdx]);
                nextIdx++;
            }

            // Read one response for the oldest in-flight packet.
            const resp = await this._reader.readResponse();

            if (resp.type === 'ack') {
                // Cumulative ACK: resp.ackSeq is the Pico's expectedSeq — the next
                // wire seq it wants, meaning every packet with a lower seq has been
                // accepted. Advance base to that point in 8-bit rolling seq space,
                // clamped to the in-flight window (< 128 « the wrap point) so a
                // duplicate ACK (delta 0) or a stale/wrapped value can neither stall
                // nor over-advance. A lost ACK self-heals via the next cumulative one.
                const delta = (resp.ackSeq - (base & 0xFF)) & 0xFF;
                if (delta > 0 && delta <= nextIdx - base) {
                    base += delta;
                    onProgress?.(base, n);
                }
            } else if (resp.type === 'nack') {
                const reasonNames = { 1: 'CRC', 2: 'FULL', 3: 'BAD_MAGIC', 4: 'PAUSED', 6: 'BAD_STATE' };
                console.warn(`[serial rx] NACK reason=${reasonNames[resp.reason] ?? resp.reason} at base=${base}`);
                if (resp.reason === NACK_BAD_MAGIC) {
                    throw new Error('Fatal: Pico reported bad magic — aborting stream.');
                }
                // Back off (longer for buffer-full) and rewind. Stale cumulative
                // ACKs still in flight need no draining — each advances base by 0
                // (or by a real accepted amount, which is correct), so they can
                // neither stall nor corrupt the window; they are matched and
                // ignored on later reads.
                const backoff = resp.reason === NACK_FULL ? 50 : 5;
                await new Promise(r => setTimeout(r, backoff));
                nextIdx = base;
            }
        }
    }

    /**
     * Send a burst of JOG packets (0xAE), one ACK per packet.
     * Only valid in STATE_IDLE or STATE_PAUSED.
     * The last segment in the burst must have MSEG_FLAG_PATH_END set.
     */
    sendJogBurst(segments) { return this._serialize(() => this._sendJogBurst(segments)); }

    async _sendJogBurst(segments) {
        for (let i = 0; i < segments.length; i++) {
            const pkt = packJog(segments[i], i & 0xFF);
            await this._writer.write(pkt);
            const resp = await this._reader.readResponse();
            if (resp.type === 'nack') {
                throw new Error(`Jog NACK reason 0x${resp.reason.toString(16)}`);
            }
        }
    }
}
